// Copyright 2015 <Magazino GmbH>

#include <pylon_camera/pylon_camera_node.h>
#include <GenApi/GenApi.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "boost/multi_array.hpp"

namespace pylon_camera
{

PylonCameraNode::PylonCameraNode() :
        nh_("~"),
        pylon_camera_(NULL),
        pylon_camera_parameter_set_(),
        it_(new image_transport::ImageTransport(nh_)),
        img_raw_pub_(it_->advertiseCamera("image_raw", 10)),
        grab_images_raw_action_server_(nh_, "grab_images_raw",
            boost::bind(&PylonCameraNode::grabImagesRawActionExecuteCB, this, _1), false),
        set_sleeping_service_(nh_.advertiseService("set_sleeping_srv", &PylonCameraNode::setSleepingCallback, this)),
        target_brightness_(-42),
        brightness_service_running_(false),
        is_sleeping_(false)
{
    init();
}

bool PylonCameraNode::init()
{
    // reading all necessary parameter to open the desired camera from the ros-parameter-server
    if (!pylon_camera_parameter_set_.readFromRosParameterServer(nh_))
    {
        ROS_ERROR("Error reading PylonCameraParameterSet from ROS-Parameter-Server");
        ros::shutdown();
        return false;
    }

    // advertising the ros-services for setting brightness, exposure, gain & gamma and
    // creating the target PylonCamera-Object with the specified device_user_id
    if (!initAndRegister())
    {
        ros::shutdown();
        return false;
    }

    if (!startGrabbing())
    {
        ros::shutdown();
        return false;
    }
    return true;
}

void PylonCameraNode::spin()
{
    // images were published if subscribers are available or if someone calls the GrabImages Action
    if (getNumSubscribers() > 0 && !isSleeping())
    {
        if (grabImage())
        {
            img_raw_pub_.publish(img_raw_msg_, cam_info_msg_);
        }
    }
}

bool PylonCameraNode::setDigitalOutputCB(const int& output_id,
                                         camera_control_msgs::SetBool::Request &req,
                                         camera_control_msgs::SetBool::Response &res)
{
    res.success = pylon_camera_->setUserOutput(output_id, req.data);
    return true;
}

const double& PylonCameraNode::desiredFrameRate() const
{
    return pylon_camera_parameter_set_.desired_frame_rate_;
}
const std::string& PylonCameraNode::cameraFrame() const
{
    return pylon_camera_parameter_set_.camera_frame_;
}
uint32_t PylonCameraNode::getNumSubscribers() const
{
    return img_raw_pub_.getNumSubscribers();
}

void PylonCameraNode::checkForPylonAutoFunctionRunning()
{
    brightness_service_running_ = pylon_camera_->isPylonAutoBrightnessFunctionRunning();
}

bool PylonCameraNode::initAndRegister()
{
    set_brightness_service_ = nh_.advertiseService("set_brightness_srv",
                                                   &PylonCameraNode::setBrightnessCallback,
                                                   this);
    set_exposure_service_ = nh_.advertiseService("set_exposure_srv",
                                                 &PylonCameraNode::setExposureCallback,
                                                 this);
    set_gain_service_ = nh_.advertiseService("set_gain_srv",
                                             &PylonCameraNode::setGainCallback,
                                             this);

    pylon_camera_ = PylonCamera::create(pylon_camera_parameter_set_.device_user_id_);

    if (pylon_camera_ == NULL)
    {
        return false;
    }

    if (!pylon_camera_->registerCameraConfiguration())
    {
        ROS_ERROR("Error while registering the camera configuration to software-trigger mode!");
        return false;
    }

    if (!pylon_camera_->openCamera())
    {
        ROS_ERROR("Error while trying to open the desired camera!");
        return false;
    }

    if (!pylon_camera_->applyStartupSettings(pylon_camera_parameter_set_))
    {
        ROS_ERROR("Error while applying the startup setting (gain, exposure, ...) to the camera!");
        return false;
    }

    if (pylon_camera_->typeName() != "DART")
    {
        set_digital_output_1_service_ = nh_.advertiseService< camera_control_msgs::SetBool::Request,
                                                              camera_control_msgs::SetBool::Response >(
                                            "set_output_1",
                                            boost::bind(&PylonCameraNode::setDigitalOutputCB, this, 1, _1, _2));
    }

    grab_images_raw_action_server_.start();
    return true;
}

bool PylonCameraNode::startGrabbing()
{
    if (!pylon_camera_->startGrabbing(pylon_camera_parameter_set_))
    {
        ROS_ERROR("Error while start grabbing");
        return false;
    }

    // Framerate Settings
    if (pylon_camera_->maxPossibleFramerate() < pylon_camera_parameter_set_.desired_frame_rate_)
    {
        ROS_INFO("Desired framerate %.2f is higher than max possible. Will limit framerate to: %.2f Hz",
                 pylon_camera_parameter_set_.desired_frame_rate_,
                 pylon_camera_->maxPossibleFramerate());
        pylon_camera_parameter_set_.desired_frame_rate_ = pylon_camera_->maxPossibleFramerate();
        nh_.setParam("desired_framerate", pylon_camera_->maxPossibleFramerate());
    }
    else if (pylon_camera_parameter_set_.desired_frame_rate_ == -1)
    {
        pylon_camera_parameter_set_.desired_frame_rate_ = pylon_camera_->maxPossibleFramerate();
        ROS_INFO("Max possible framerate is %.2f Hz", pylon_camera_->maxPossibleFramerate());
    }

    // setting up the CameraInfo object with the data of the uncalibrated image
    setupCameraInfo(cam_info_msg_);

    img_raw_msg_.header.frame_id = cameraFrame();
    // Encoding of pixels -- channel meaning, ordering, size
    // taken from the list of strings in include/sensor_msgs/image_encodings.h
    img_raw_msg_.encoding = pylon_camera_->imageEncoding();
    img_raw_msg_.height = pylon_camera_->imageRows();
    img_raw_msg_.width = pylon_camera_->imageCols();

    // step = full row length in bytes
    // img_raw_msg_.data // actual matrix data, size is (step * rows)
    img_raw_msg_.step = img_raw_msg_.width * pylon_camera_->imagePixelDepth();

    return true;
}

void PylonCameraNode::setupCameraInfo(sensor_msgs::CameraInfo& cam_info_msg)
{
    std_msgs::Header header;
    header.frame_id = cameraFrame();
    header.stamp = ros::Time::now();

    // http://www.ros.org/reps/rep-0104.html
    // If the camera is uncalibrated, the matrices D, K, R, P should be left zeroed out.
    // In particular, clients may assume that K[0] == 0.0 indicates an uncalibrated camera.
    cam_info_msg.header = header;
    cam_info_msg.height = pylon_camera_->imageRows();
    cam_info_msg.width = pylon_camera_->imageCols();

    // The distortion model used. Supported models are listed in sensor_msgs/distortion_models.h.
    // For most cameras, "plumb_bob" - a simple model of radial and tangential distortion - is sufficient.
    // Empty D and distortion_model indicate that the CameraInfo cannot be used to rectify points or images,
    // either because the camera is not calibrated or because the rectified image was produced using an
    // unsupported distortion model, e.g. the proprietary one used by Bumblebee cameras
    // [http://www.ros.org/reps/rep-0104.html].
    cam_info_msg.distortion_model = "";

    // The distortion parameters, size depending on the distortion model.
    // For "plumb_bob", the 5 parameters are: (k1, k2, t1, t2, k3) -> float64[] D.
    cam_info_msg.D = std::vector<double>(5, 0.);

    // Intrinsic camera matrix for the raw (distorted) images.
    //     [fx  0 cx]
    // K = [ 0 fy cy]  --> 3x3 row-major matrix
    //     [ 0  0  1]
    // Projects 3D points in the camera coordinate frame to 2D pixel coordinates using the
    // focal lengths (fx, fy) and principal point (cx, cy).
    cam_info_msg.K.assign(0.0);

    // Rectification matrix (stereo cameras only)
    // A rotation matrix aligning the camera coordinate system to the ideal stereo image plane so that
    // epipolar lines in both stereo images are parallel.
    cam_info_msg.R.assign(0.0);

    // Projection/camera matrix
    //     [fx'  0  cx' Tx]
    // P = [ 0  fy' cy' Ty]  --> # 3x4 row-major matrix
    //     [ 0   0   1   0]
    // By convention, this matrix specifies the intrinsic (camera) matrix of the processed (rectified) image.
    // That is, the left 3x3 portion is the normal camera intrinsic matrix for the rectified image.
    // It projects 3D points in the camera coordinate frame to 2D pixel coordinates using the focal
    // lengths (fx', fy') and principal point (cx', cy') - these may differ from the values in K.
    // For monocular cameras, Tx = Ty = 0. Normally, monocular cameras will also have R = the identity
    // and P[1:3,1:3] = K.
    // For a stereo pair, the fourth column [Tx Ty 0]' is related to the position of the optical center of
    // The second camera in the first camera's frame. We assume Tz = 0 so both cameras are in the same
    // stereo image plane. The first camera always has Tx = Ty = 0. For the right (second) camera of a
    // horizontal stereo pair, Ty = 0 and Tx = -fx' * B, where B is the baseline between the cameras.
    // Given a 3D point [X Y Z]', the projection (x, y) of the point onto the rectified image is given by:
    // [u v w]' = P * [X Y Z 1]'
    //        x = u / w
    //        y = v / w
    //  This holds for both images of a stereo pair.
    cam_info_msg.P.assign(0.0);

    // Binning refers here to any camera setting which combines rectangular neighborhoods of pixels into
    // larger "super-pixels." It reduces the resolution of the output image to
    // (width / binning_x) x (height / binning_y). The default values binning_x = binning_y = 0 is
    // considered the same as binning_x = binning_y = 1 (no subsampling).
    cam_info_msg.binning_x = cam_info_msg.binning_y = pylon_camera_parameter_set_.binning_;

    // Region of interest (subwindow of full camera resolution), given in full resolution (unbinned)
    // image coordinates. A particular ROI always denotes the same window of pixels on the camera sensor,
    // regardless of binning settings. The default setting of roi (all values 0) is considered the same as
    // full resolution (roi.width = width, roi.height = height).
    cam_info_msg.roi.x_offset = cam_info_msg.roi.y_offset = 0;
    cam_info_msg.roi.height = cam_info_msg.roi.width = 0;
}

/**
 * Waits till the pylon_camera_ isReady() observing a given timeout
 * @return true when the camera's state toggles to 'isReady()'
 */
bool PylonCameraNode::waitForCamera(const ros::Duration& timeout) const
{
    bool result = false;
    ros::Time start_time = ros::Time::now();

    while (ros::ok())
    {
        if (pylon_camera_->isReady())
        {
            result = true;
            break;
        }
        else
        {
            if (timeout >= ros::Duration(0))
            {
                if (ros::Time::now() - start_time >= timeout)
                {
                    ROS_ERROR_STREAM("Setting brightness failed, because the interface is not ready." <<
                        "This happens although waiting for " << timeout.sec << " seconds!");
                    return false;
                }
            }
            ros::Duration(0.02).sleep();
        }
    }
    return result;
}

bool PylonCameraNode::grabImage()
{
    boost::lock_guard<boost::recursive_mutex> lock(grab_mutex_);
    if (!pylon_camera_->grab(img_raw_msg_.data))
    {
        if (pylon_camera_->isCamRemoved())
        {
            ROS_ERROR("Pylon camera has been removed!");
            ros::shutdown();
        }
        else
        {
            ROS_WARN("Pylon camera returned invalid image! Skipping");
        }
        return false;
    }
    img_raw_msg_.header.stamp = ros::Time::now();
    cam_info_msg_.header.stamp = img_raw_msg_.header.stamp;
    return true;
}

void PylonCameraNode::grabImagesRawActionExecuteCB(const camera_control_msgs::GrabImagesGoal::ConstPtr& goal)
{
    camera_control_msgs::GrabImagesResult result;
    camera_control_msgs::GrabImagesFeedback feedback;

    boost::lock_guard<boost::recursive_mutex> lock(grab_mutex_);

    result.images.resize(goal->target_values.size());
    result.reached_values.resize(goal->target_values.size());
    result.success = true;
    for (std::size_t i = 0; i < goal->target_values.size(); ++i)
    {
        // setGain(goal->gain);
        // setGamma(goal->gamma);
        if (goal->target_type == goal->EXPOSURE)
        {
            setExposure(goal->target_values[i], result.reached_values[i]);
        }
        if (goal->target_type == goal->BRIGHTNESS)
        {
            int reached_val;
            setBrightness(goal->target_values[i], reached_val);
            result.reached_values[i] = static_cast<float>(reached_val);
        }

        sensor_msgs::Image& img = result.images[i];
        img.encoding = pylon_camera_->imageEncoding();
        img.height = pylon_camera_->imageRows();
        img.width = pylon_camera_->imageCols();
        // step = full row length in bytes
        img.step = img.width * pylon_camera_->imagePixelDepth();

        if (!pylon_camera_->grab(img.data))
        {
            result.success = false;
        }
        img.header.stamp = ros::Time::now();
        img.header.frame_id = cameraFrame();
        feedback.curr_nr_images_taken = i+1;
        grab_images_raw_action_server_.publishFeedback(feedback);
    }

    if (!result.success)
    {
        result.images.clear();
    }

    grab_images_raw_action_server_.setSucceeded(result);
}

bool PylonCameraNode::setExposure(const float& target_exposure, float& reached_exposure)
{
    boost::lock_guard<boost::recursive_mutex> lock(grab_mutex_);
    if (!pylon_camera_->isReady())
    {
        ROS_WARN("Error in setExposure(): pylon_camera_ is not ready!");
        return false;
    }

    reached_exposure = getCurrentExposure();

    if (reached_exposure != target_exposure)
    {
        pylon_camera_->setExposure(target_exposure);
    }

    // wait for max 5s till the cam has updated the exposure
    ros::Rate r(10.0);
    ros::Time start = ros::Time::now();
    while (ros::ok())
    {
        reached_exposure = getCurrentExposure();

        bool success = fabs(reached_exposure - target_exposure) < pylon_camera_->exposureStep();

        if (success)
        {
            return true;
        }

        if (ros::Time::now() - start > ros::Duration(5.0))
        {
            ROS_ERROR("Error in setExposure(): Did not reach the desired brightness in time");
            return false;
        }
        r.sleep();
    }
    return true;
}

bool PylonCameraNode::setExposureCallback(camera_control_msgs::SetExposureSrv::Request &req,
                                          camera_control_msgs::SetExposureSrv::Response &res)
{
    res.success = setExposure(req.target_exposure, res.reached_exposure);
    return true;
}

bool PylonCameraNode::setBrightness(const int& target_brightness, int& reached_brightness)
{
    boost::lock_guard<boost::recursive_mutex> lock(grab_mutex_);

    // brightness service can only work, if an image has already been grabbed, because it calculates the mean on the
    // current image. The interface is ready if the grab-result-pointer of the first acquisition contains valid data
    if (!waitForCamera(ros::Duration(3.0)))
    {
        ROS_ERROR("Setting brightness failed: interface not ready, although waiting for 3 sec!");
        return false;
    }

    // get actual image -> fills img_raw_msg_.data vector
    if (!grabImage())
    {
        ROS_ERROR("Failed to grab image, can't calculate current brightness!");
        return false;
    }

    // calculates current brightness by generating the mean over all pixels
    // stored in img_raw_msg_.data vector
    float current_brightness = calcCurrentBrightness();

    ROS_INFO_STREAM("New brightness request for target brightness "
                    << target_brightness << ", current brightness = "
                    << current_brightness);

    if ( fabs(current_brightness - static_cast<float>(target_brightness)) <= 1.0)
    {
        reached_brightness = static_cast<int>(current_brightness);
        ROS_INFO("Desired brightness already reached");
        return true;  // target brightness already reached
    }

    // timeout for the brightness search -> need more time for great exposure values
    ros::Duration timeout;
    target_brightness > 205 ? timeout = ros::Duration(15.0) : timeout = ros::Duration(5.0);

    ROS_INFO("Now going into while loop...");
    bool is_brightness_reached = false;
    size_t fail_safe_ctr = 0;
    float last_brightness = std::numeric_limits<float>::max();
    ros::Time start = ros::Time::now();
    while ( ros::ok() )
    {
        // calling setBrightness in every cycle would not be necessary for the pylon auto
        // brightness search. But for the case that the target brightness is out of the
        // pylon range which is from [50 - 205] a binary exposure search will be executed
        // where we have to update the search parameter in every cycle
        //if (!
        pylon_camera_->setBrightness(target_brightness, current_brightness);
        //{
        //    ROS_ERROR("Error while setting target brightness!");
        //    pylon_camera_->disableAllRunningAutoBrightessFunctions();
        //    break;
        //}

        grabImage();

        current_brightness = calcCurrentBrightness();

        is_brightness_reached = fabs(current_brightness - static_cast<float>(target_brightness))
                                < pylon_camera_->maxBrightnessTolerance();
        if (is_brightness_reached)
        {
            pylon_camera_->disableAllRunningAutoBrightessFunctions();
            ROS_INFO("Brightness reached: %.3f", current_brightness);
            break;
        }

        if (ros::Time::now() - start > timeout)
        {
            // cancel all running brightness search by deactivating ExposureAuto
            pylon_camera_->disableAllRunningAutoBrightessFunctions();
            ROS_ERROR_STREAM("Did not reach the target brightness before timeout "
                    << timeout.sec << " sec, stuck at " << current_brightness);
            break;
        }

        if (fabs(last_brightness - current_brightness) <= 1.0)
        {
            fail_safe_ctr++;
        }
        last_brightness = current_brightness;

        if (fail_safe_ctr > 5)
        {
            //ROS_ERROR_STREAM("Seems like the desired brightness (" << target_brightness
            //        << ") is not reachable with the current gain (" << getCurrentGain()
            //        << ")! Stuck at brighntness "<< current_brightness);
            break;
        }

        if (pylon_camera_->isBrightnessSearchRunning())
        {
            //ROS_INFO_STREAM("BS running: Current br = " << current_brightness
            //        << ", Current Gain = " << getCurrentGain()
            //        << ", Current Exp = " << getCurrentExposure()
            //        << ", Current Limits = [" << getCurrentAutoExposureTimeLowerLimit()
            //        << ", " << getCurrentAutoExposureTimeUpperLimit()
            //        << "]");
            //ROS_INFO("cam auto value = %f", pylon_camera_->autoTargetBrightness().GetValue());
        }
        else
        {
            ROS_INFO("Brightness Search --- NOT --- running, FINAL BRIGHTNESS = %.3f", current_brightness);
        }
    }

    reached_brightness = static_cast<int>(current_brightness);
    ROS_INFO("Final Brightness: %d", reached_brightness);

    std::cout << "fabs = " << fabs(current_brightness - target_brightness) << std::endl;
    std::cout << "max tolerance = " << pylon_camera_->maxBrightnessTolerance() << std::endl;
    std::cout << "bool = " << is_brightness_reached << std::endl;
    return is_brightness_reached;

}

bool PylonCameraNode::setBrightnessCallback(camera_control_msgs::SetBrightnessSrv::Request &req,
    camera_control_msgs::SetBrightnessSrv::Response &res)
{
    res.success = setBrightness(req.target_brightness, res.reached_brightness);
    return true;
}

float PylonCameraNode::calcCurrentBrightness()
{
    boost::lock_guard<boost::recursive_mutex> lock(grab_mutex_);
    assert(img_raw_msg_.data.size() > 0);
    int sum = std::accumulate(img_raw_msg_.data.begin(), img_raw_msg_.data.end(), 0);
    float mean = static_cast<int>(sum) / img_raw_msg_.data.size();
    return mean;
}

float PylonCameraNode::getCurrentExposure()
{
    return pylon_camera_->currentExposure();
}

float PylonCameraNode::getCurrentGain()
{
    return pylon_camera_->currentGain();
}

float PylonCameraNode::getCurrentAutoExposureTimeLowerLimit()
{
    return pylon_camera_->currentAutoExposureTimeLowerLimit();
}

float PylonCameraNode::getCurrentAutoExposureTimeUpperLimit()
{
    return pylon_camera_->currentAutoExposureTimeUpperLimit();
}

bool PylonCameraNode::setGain(const float& target_gain, float& reached_gain)
{
    boost::lock_guard<boost::recursive_mutex> lock(grab_mutex_);
    if (!pylon_camera_->isReady())
    {
        ROS_WARN("Error in setGain(): pylon_camera_ is not ready!");
        return false;
    }

    reached_gain = getCurrentGain();

    if (reached_gain != target_gain)
    {
        pylon_camera_->setGain(target_gain);
    }

    // wait for max 5s till the cam has updated the gain
    ros::Rate r(10.0);
    ros::Time start = ros::Time::now();
    while (ros::ok())
    {
        reached_gain = getCurrentGain();

        bool success = fabs(reached_gain - target_gain) < 0.01; // Delta of 1% is allowed

        if (success)
        {
            return true;
        }

        if (ros::Time::now() - start > ros::Duration(5.0))
        {
            ROS_ERROR("Error in setGain(): Did not reach the desired gain in time");
            return false;
        }
        r.sleep();
    }
    return true;
}

bool PylonCameraNode::setGainCallback(camera_control_msgs::SetGain::Request &req,
                                      camera_control_msgs::SetGain::Response &res)
{
    res.success = setGain(req.target_gain, res.reached_gain);
    return true;
}

bool PylonCameraNode::setSleepingCallback(camera_control_msgs::SetSleepingSrv::Request &req,
    camera_control_msgs::SetSleepingSrv::Response &res)
{
    is_sleeping_ = req.set_sleeping;

    if (is_sleeping_)
    {
        ROS_INFO("Seting Pylon Camera Node to sleep...");
    }
    else
    {
        ROS_INFO("Pylon Camera Node continues grabbing");
    }

    res.success = true;
    return true;
}

bool PylonCameraNode::isSleeping()
{
    return is_sleeping_;
}

PylonCameraNode::~PylonCameraNode()
{
    delete pylon_camera_;
    pylon_camera_ = NULL;
    delete it_;
    it_ = NULL;
}

}  // namespace pylon_camera
