/*
Copyright (c) 2010-2016, Mathieu Labbe - IntRoLab - Universite de Sherbrooke
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Universite de Sherbrooke nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <rtabmap/core/camera/CameraRealSense2.h>
#include <rtabmap/utilite/UTimer.h>
#include <rtabmap/utilite/UThreadC.h>
#include <rtabmap/utilite/UConversion.h>
#include <rtabmap/utilite/UEventsManager.h>
#include <opencv2/imgproc/types_c.h>

#ifdef RTABMAP_REALSENSE2
#include <librealsense2/rs.hpp>
#include <librealsense2/rsutil.h>
#include <librealsense2/hpp/rs_processing.hpp>
#include <librealsense2/rs_advanced_mode.hpp>
#include <fstream>
#endif

namespace rtabmap
{

bool CameraRealSense2::available()
{
#ifdef RTABMAP_REALSENSE2
	return true;
#else
	return false;
#endif
}

CameraRealSense2::CameraRealSense2(
		const std::string & device,
		float imageRate,
		const rtabmap::Transform & localTransform) :
	Camera(imageRate, localTransform)
#ifdef RTABMAP_REALSENSE2
    ,
	ctx_(new rs2::context),
	dev_(2, 0),
	deviceId_(device),
	syncer_(new rs2::syncer),
	depth_scale_meters_(1.0f),
	depthIntrinsics_(new rs2_intrinsics),
	rgbIntrinsics_(new rs2_intrinsics),
	depthToRGBExtrinsics_(new rs2_extrinsics),
	lastImuStamp_(0.0),
	clockSyncWarningShown_(false),
	imuGlobalSyncWarningShown_(false),
	emitterEnabled_(true),
	ir_(false),
	irDepth_(true),
	rectifyImages_(true),
	odometryProvided_(false),
	cameraWidth_(640),
	cameraHeight_(480),
	cameraFps_(30),
	globalTimeSync_(true),
	publishInterIMU_(false),
	dualMode_(false),
	closing_(false),
	isL500_(false)
#endif
{
	UDEBUG("");
}

CameraRealSense2::~CameraRealSense2()
{
#ifdef RTABMAP_REALSENSE2
	closing_ = true;
	try
	{
		UDEBUG("Closing device(s)...");
		for(size_t i=0; i<dev_.size(); ++i)
		{
			if(dev_[i])
			{
				UDEBUG("Closing %d sensor(s) from device %d...", (int)dev_[i]->query_sensors().size(), (int)i);
				for(rs2::sensor _sensor : dev_[i]->query_sensors())
				{
					try
					{
						_sensor.stop();
						_sensor.close();
					}
					catch(const rs2::error & error)
					{
						UWARN("%s", error.what());
					}
				}
				dev_[i]->hardware_reset(); // To avoid freezing on some Windows computers in the following destructor
				delete dev_[i];
			}
		}
	}
	catch(const rs2::error & error)
	{
		UINFO("%s", error.what());
	}
	try {
		delete ctx_;
	}
	catch(const rs2::error & error)
	{
		UWARN("%s", error.what());
	}
	try {
		delete syncer_;
	}
	catch(const rs2::error & error)
	{
		UWARN("%s", error.what());
	}
	delete depthIntrinsics_;
	delete rgbIntrinsics_;
	delete depthToRGBExtrinsics_;
#endif
}

#ifdef RTABMAP_REALSENSE2
void CameraRealSense2::imu_callback(rs2::frame frame)
{
	auto stream = frame.get_profile().stream_type();
	cv::Vec3f crnt_reading = *reinterpret_cast<const cv::Vec3f*>(frame.get_data());
	UDEBUG("%s callback! %f (%f %f %f)",
			stream == RS2_STREAM_GYRO?"GYRO":"ACC",
			frame.get_timestamp(),
			crnt_reading[0],
			crnt_reading[1],
			crnt_reading[2]);
	UScopeMutex sm(imuMutex_);
	if(stream == RS2_STREAM_GYRO)
	{
		gyroBuffer_.insert(gyroBuffer_.end(), std::make_pair(frame.get_timestamp(), crnt_reading));
		if(gyroBuffer_.size() > 1000)
		{
			gyroBuffer_.erase(gyroBuffer_.begin());
		}
	}
	else
	{
		accBuffer_.insert(accBuffer_.end(), std::make_pair(frame.get_timestamp(), crnt_reading));
		if(accBuffer_.size() > 1000)
		{
			accBuffer_.erase(accBuffer_.begin());
		}
	}
}

// See https://github.com/IntelRealSense/realsense-ros/blob/2a45f09003c98a5bdf39ee89df032bdb9c9bcd2d/realsense2_camera/src/base_realsense_node.cpp#L1397-L1404
Transform CameraRealSense2::realsense2PoseRotation_ = Transform(
		0, 0,-1,0,
		-1, 0, 0,0,
		 0, 1, 0,0);

void CameraRealSense2::pose_callback(rs2::frame frame)
{
	rs2_pose pose = frame.as<rs2::pose_frame>().get_pose_data();
	// See https://github.com/IntelRealSense/realsense-ros/blob/2a45f09003c98a5bdf39ee89df032bdb9c9bcd2d/realsense2_camera/src/base_realsense_node.cpp#L1397-L1404
	Transform poseT = Transform(
				-pose.translation.z,
				-pose.translation.x,
				pose.translation.y,
				-pose.rotation.z,
				-pose.rotation.x,
				pose.rotation.y,
				pose.rotation.w);

	UDEBUG("POSE callback! %f %s (confidence=%d)", frame.get_timestamp(), poseT.prettyPrint().c_str(), (int)pose.tracker_confidence);

	UScopeMutex sm(poseMutex_);
	poseBuffer_.insert(poseBuffer_.end(), std::make_pair(frame.get_timestamp(), std::make_pair(poseT, pose.tracker_confidence)));
	if(poseBuffer_.size() > 1000)
	{
		poseBuffer_.erase(poseBuffer_.begin());
	}
}

void CameraRealSense2::frame_callback(rs2::frame frame)
{
	UDEBUG("Frame callback! %f", frame.get_timestamp());
	(*syncer_)(frame);
}
void CameraRealSense2::multiple_message_callback(rs2::frame frame)
{
    auto stream = frame.get_profile().stream_type();
    switch (stream)
    {
        case RS2_STREAM_GYRO:
        case RS2_STREAM_ACCEL:
        	//UWARN("IMU : Domain=%d time=%f host=%f", frame.get_frame_timestamp_domain(), frame.get_timestamp()/1000.0, UTimer::now());
            imu_callback(frame);
            break;
        case RS2_STREAM_POSE:
        	//UWARN("POSE : Domain=%d time=%f host=%f", frame.get_frame_timestamp_domain(), frame.get_timestamp()/1000.0, UTimer::now());
        	if(odometryProvided_)
        	{
        		pose_callback(frame);
        	}
            break;
        default:
        	//UWARN("IMG : Domain=%d time=%f host=%f", frame.get_frame_timestamp_domain(), frame.get_timestamp()/1000.0, UTimer::now());
            frame_callback(frame);
    }
}

void CameraRealSense2::getPoseAndIMU(
		const double & stamp,
		Transform & pose,
		unsigned int & poseConfidence,
		IMU & imu,
		int maxWaitTimeMs)
{
	pose.setNull();
	imu = IMU();
	poseConfidence = 0;
	if(accBuffer_.empty() || gyroBuffer_.empty())
	{
		return;
	}

	// Interpolate pose
	if(!poseBuffer_.empty())
	{
		poseMutex_.lock();
		int waitTry = 0;
		while(maxWaitTimeMs>0 && poseBuffer_.rbegin()->first < stamp && waitTry < maxWaitTimeMs)
		{
			poseMutex_.unlock();
			++waitTry;
			uSleep(1);
			poseMutex_.lock();
		}
		if(poseBuffer_.rbegin()->first < stamp)
		{
			if(maxWaitTimeMs > 0)
			{
				UWARN("Could not find poses to interpolate at image time %f after waiting %d ms (last is %f)...", stamp, maxWaitTimeMs, poseBuffer_.rbegin()->first);
			}
		}
		else
		{
			std::map<double, std::pair<Transform, unsigned int> >::const_iterator iterB = poseBuffer_.lower_bound(stamp);
			std::map<double, std::pair<Transform, unsigned int> >::const_iterator iterA = iterB;
			if(iterA != poseBuffer_.begin())
			{
				iterA = --iterA;
			}
			if(iterB == poseBuffer_.end())
			{
				iterB = --iterB;
			}
			if(iterA == iterB && stamp == iterA->first)
			{
				pose = iterA->second.first;
				poseConfidence = iterA->second.second;
			}
			else if(stamp >= iterA->first && stamp <= iterB->first)
			{
				pose = iterA->second.first.interpolate((stamp-iterA->first) / (iterB->first-iterA->first), iterB->second.first);
				poseConfidence = iterA->second.second;
			}
			else if(stamp < iterA->first)
			{
				UWARN("Could not find poses to interpolate at image time %f (earliest is %f). Are sensors synchronized?", stamp, iterA->first);
			}
			else
			{
				UWARN("Could not find poses to interpolate at image time %f (between %f and %f), Are sensors synchronized?", stamp, iterA->first, iterB->first);
			}
		}
		poseMutex_.unlock();
	}

	// Interpolate acc
	cv::Vec3d acc;
	{
		imuMutex_.lock();
		int waitTry = 0;
		while(maxWaitTimeMs > 0 && accBuffer_.rbegin()->first < stamp && waitTry < maxWaitTimeMs)
		{
			imuMutex_.unlock();
			++waitTry;
			uSleep(1);
			imuMutex_.lock();
		}
		if(accBuffer_.rbegin()->first < stamp)
		{
			if(maxWaitTimeMs>0)
			{
				UWARN("Could not find acc data to interpolate at image time %f after waiting %d ms (last is %f)...", stamp, maxWaitTimeMs, accBuffer_.rbegin()->first);
			}
			imuMutex_.unlock();
			return;
		}
		else
		{
			std::map<double, cv::Vec3f>::const_iterator iterB = accBuffer_.lower_bound(stamp);
			std::map<double, cv::Vec3f>::const_iterator iterA = iterB;
			if(iterA != accBuffer_.begin())
			{
				iterA = --iterA;
			}
			if(iterB == accBuffer_.end())
			{
				iterB = --iterB;
			}
			if(iterA == iterB && stamp == iterA->first)
			{
				acc[0] = iterA->second[0];
				acc[1] = iterA->second[1];
				acc[2] = iterA->second[2];
			}
			else if(stamp >= iterA->first && stamp <= iterB->first)
			{
				float t = (stamp-iterA->first) / (iterB->first-iterA->first);
				acc[0] = iterA->second[0] + t*(iterB->second[0] - iterA->second[0]);
				acc[1] = iterA->second[1] + t*(iterB->second[1] - iterA->second[1]);
				acc[2] = iterA->second[2] + t*(iterB->second[2] - iterA->second[2]);
			}
			else
			{
				if(!imuGlobalSyncWarningShown_)
				{
					if(stamp < iterA->first)
					{
						UWARN("Could not find acc data to interpolate at image time %f (earliest is %f). Are sensors synchronized?", stamp, iterA->first);
					}
					else
					{
						UWARN("Could not find acc data to interpolate at image time %f (between %f and %f). Are sensors synchronized?", stamp, iterA->first, iterB->first);
					}
				}
				if(!globalTimeSync_)
				{
					if(!imuGlobalSyncWarningShown_)
					{
						UWARN("As globalTimeSync option is off, the received gyro and accelerometer will be re-stamped with image time. This message is only shown once.");
						imuGlobalSyncWarningShown_ = true;
					}
					std::map<double, cv::Vec3f>::const_reverse_iterator iterC = accBuffer_.rbegin();
					acc[0] = iterC->second[0];
					acc[1] = iterC->second[1];
					acc[2] = iterC->second[2];
				}
				else
				{
					imuMutex_.unlock();
					return;
				}
			}
		}
		imuMutex_.unlock();
	}

	// Interpolate gyro
	cv::Vec3d gyro;
	{
		imuMutex_.lock();
		int waitTry = 0;
		while(maxWaitTimeMs>0 && gyroBuffer_.rbegin()->first < stamp && waitTry < maxWaitTimeMs)
		{
			imuMutex_.unlock();
			++waitTry;
			uSleep(1);
			imuMutex_.lock();
		}
		if(gyroBuffer_.rbegin()->first < stamp)
		{
			if(maxWaitTimeMs>0)
			{
				UWARN("Could not find gyro data to interpolate at image time %f after waiting %d ms (last is %f)...", stamp, maxWaitTimeMs, gyroBuffer_.rbegin()->first);
			}
			imuMutex_.unlock();
			return;
		}
		else
		{
			std::map<double, cv::Vec3f>::const_iterator iterB = gyroBuffer_.lower_bound(stamp);
			std::map<double, cv::Vec3f>::const_iterator iterA = iterB;
			if(iterA != gyroBuffer_.begin())
			{
				iterA = --iterA;
			}
			if(iterB == gyroBuffer_.end())
			{
				iterB = --iterB;
			}
			if(iterA == iterB && stamp == iterA->first)
			{
				gyro[0] = iterA->second[0];
				gyro[1] = iterA->second[1];
				gyro[2] = iterA->second[2];
			}
			else if(stamp >= iterA->first && stamp <= iterB->first)
			{
				float t = (stamp-iterA->first) / (iterB->first-iterA->first);
				gyro[0] = iterA->second[0] + t*(iterB->second[0] - iterA->second[0]);
				gyro[1] = iterA->second[1] + t*(iterB->second[1] - iterA->second[1]);
				gyro[2] = iterA->second[2] + t*(iterB->second[2] - iterA->second[2]);
			}
			else
			{
				if(!imuGlobalSyncWarningShown_)
				{
					if(stamp < iterA->first)
					{
						UWARN("Could not find gyro data to interpolate at image time %f (earliest is %f). Are sensors synchronized?", stamp, iterA->first);
					}
					else
					{
						UWARN("Could not find gyro data to interpolate at image time %f (between %f and %f). Are sensors synchronized?", stamp, iterA->first, iterB->first);
					}
				}
				if(!globalTimeSync_)
				{
					if(!imuGlobalSyncWarningShown_)
					{
						UWARN("As globalTimeSync option is off, the latest received gyro and accelerometer will be re-stamped with image time. This message is only shown once.");
						imuGlobalSyncWarningShown_ = true;
					}
					std::map<double, cv::Vec3f>::const_reverse_iterator iterC = gyroBuffer_.rbegin();
					gyro[0] = iterC->second[0];
					gyro[1] = iterC->second[1];
					gyro[2] = iterC->second[2];
				}
				else
				{
					imuMutex_.unlock();
					return;
				}
			}
		}
		imuMutex_.unlock();
	}

	imu = IMU(gyro, cv::Mat::eye(3, 3, CV_64FC1), acc, cv::Mat::eye(3, 3, CV_64FC1), imuLocalTransform_);
}
#endif

bool CameraRealSense2::init(const std::string & calibrationFolder, const std::string & cameraName)
{
	UDEBUG("");
#ifdef RTABMAP_REALSENSE2

	UINFO("setupDevice...");

	for(size_t i=0; i<dev_.size(); ++i)
	{
		delete dev_[i];
		dev_[i] = 0;
	}
	clockSyncWarningShown_ = false;
	imuGlobalSyncWarningShown_ = false;

	auto list = ctx_->query_devices();
	if (0 == list.size())
	{
		UERROR("No RealSense2 devices were found!");
		return false;
	}

	bool found=false;
	for (auto&& dev : list)
	{
		auto sn = dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
		auto pid_str = dev.get_info(RS2_CAMERA_INFO_PRODUCT_ID);
		uint16_t pid;
		std::stringstream ss;
		ss << std::hex << pid_str;
		ss >> pid;
		UINFO("Device with serial number %s was found with product ID=%d.", sn, (int)pid);
		if(dualMode_ && pid == 0x0B37)
		{
			// Dual setup: device[0] = D400, device[1] = T265
			// T265
			dev_[1] = new rs2::device();
			*dev_[1] = dev;
		}
		else if (!found && (deviceId_.empty() || deviceId_ == sn))
		{
			dev_[0] = new rs2::device();
			*dev_[0] = dev;
			found=true;
		}
	}

	if (!found)
	{
		if(dualMode_ && dev_[1]!=0)
		{
			UERROR("Dual setup is enabled, but a D400 camera is not detected!");
			delete dev_[1];
			dev_[1] = 0;
		}
		else
		{
			UERROR("The requested device \"%s\" is NOT found!", deviceId_.c_str());
		}
		return false;
	}
	else if(dualMode_ && dev_[1] == 0)
	{
		UERROR("Dual setup is enabled, but a T265 camera is not detected!");
		delete dev_[0];
		dev_[0] = 0;
		return false;
	}

	if (!jsonConfig_.empty())
	{
		if (dev_[0]->is<rs400::advanced_mode>())
		{
			std::stringstream ss;
			std::ifstream in(jsonConfig_);
			if (in.is_open())
			{
				ss << in.rdbuf();
				std::string json_file_content = ss.str();

				auto adv = dev_[0]->as<rs400::advanced_mode>();
				adv.load_json(json_file_content);
				UINFO("JSON file is loaded! (%s)", jsonConfig_.c_str());
			}
			else
			{
				UWARN("JSON file provided doesn't exist! (%s)", jsonConfig_.c_str());
			}
		}
		else
		{
			UWARN("A json config file is provided (%s), but device does not support advanced settings!", jsonConfig_.c_str());
		}
	}

	ctx_->set_devices_changed_callback([this](rs2::event_information& info)
	{
		for(size_t i=0; i<dev_.size(); ++i)
		{
			if(dev_[i])
			{
				if (info.was_removed(*dev_[i]))
				{
					if (closing_)
					{
						UDEBUG("The device %d has been disconnected!", i);
					}
					else
					{
						UERROR("The device %d has been disconnected!", i);
					}
				}
			}
		}
	});


	auto camera_name = dev_[0]->get_info(RS2_CAMERA_INFO_NAME);
	UINFO("Device Name: %s", camera_name);

	auto sn = dev_[0]->get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
	UINFO("Device Serial No: %s", sn);

	auto fw_ver = dev_[0]->get_info(RS2_CAMERA_INFO_FIRMWARE_VERSION);
	UINFO("Device FW version: %s", fw_ver);

	auto pid = dev_[0]->get_info(RS2_CAMERA_INFO_PRODUCT_ID);
	UINFO("Device Product ID: 0x%s", pid);

	auto dev_sensors = dev_[0]->query_sensors();
	if(dualMode_)
	{
		auto dev_sensors2 = dev_[1]->query_sensors();
		dev_sensors.insert(dev_sensors.end(), dev_sensors2.begin(), dev_sensors2.end());
	}

	UINFO("Device Sensors: ");
	std::vector<rs2::sensor> sensors(2); //0=rgb 1=depth 2=(pose in dualMode_)
	bool stereo = false;
	isL500_ = false;
	for(auto&& elem : dev_sensors)
	{
		std::string module_name = elem.get_info(RS2_CAMERA_INFO_NAME);
		if ("Stereo Module" == module_name)
		{
			sensors[1] = elem;
			sensors[1].set_option(rs2_option::RS2_OPTION_EMITTER_ENABLED, emitterEnabled_);
		}
		else if ("Coded-Light Depth Sensor" == module_name)
		{
		}
		else if ("RGB Camera" == module_name)
		{
			if(!ir_)
			{
				sensors[0] = elem;
			}
		}
		else if ("Wide FOV Camera" == module_name)
		{
		}
		else if ("Motion Module" == module_name)
		{
			if(!dualMode_)
			{
				sensors.resize(3);
				sensors[2] = elem;
			}
		}
		else if ("Tracking Module" == module_name)
		{
			if(dualMode_)
			{
				sensors.resize(3);
			}
			else
			{
				sensors.resize(1);
				stereo = true;
			}
			sensors.back() = elem;
			sensors.back().set_option(rs2_option::RS2_OPTION_ENABLE_POSE_JUMPING, 0);
			sensors.back().set_option(rs2_option::RS2_OPTION_ENABLE_RELOCALIZATION, 0);
		}
		else if ("L500 Depth Sensor" == module_name)
		{
			sensors[1] = elem;
			isL500_ = true;
		}
		else
		{
			UERROR("Module Name \"%s\" isn't supported!", module_name.c_str());
			return false;
		}
		UINFO("%s was found.", elem.get_info(RS2_CAMERA_INFO_NAME));
	}

	UDEBUG("");

	model_ = CameraModel();
	rs2::stream_profile depthStreamProfile;
	rs2::stream_profile rgbStreamProfile;
	std::vector<std::vector<rs2::stream_profile> > profilesPerSensor(sensors.size());
	 for (unsigned int i=0; i<sensors.size(); ++i)
	 {
		if(i==0 && ir_ && !stereo)
		{
			continue;
		}
		UINFO("Sensor %d \"%s\"", (int)i, sensors[i].get_info(RS2_CAMERA_INFO_NAME));
		auto profiles = sensors[i].get_stream_profiles();
		bool added = false;
		UINFO("profiles=%d", (int)profiles.size());
		if(ULogger::level()>=ULogger::kInfo)
		{
			for (auto& profile : profiles)
			{
				auto video_profile = profile.as<rs2::video_stream_profile>();
				UINFO("%s %d %d %d %d %s type=%d", rs2_format_to_string(
						video_profile.format()),
						video_profile.width(),
						video_profile.height(),
						video_profile.fps(),
						video_profile.stream_index(),
						video_profile.stream_name().c_str(),
						video_profile.stream_type());
			}
		}
		int pi = 0;
		for (auto& profile : profiles)
		{
			auto video_profile = profile.as<rs2::video_stream_profile>();
			if(!stereo)
			{
				if(isL500_ &&
						(video_profile.width()  == 640 &&
						 video_profile.height() == 480 &&
						 video_profile.fps()    == 30))
				{
					if( i==0 // rgb
						&& video_profile.format() == RS2_FORMAT_RGB8 && video_profile.stream_type() == RS2_STREAM_COLOR)
					{
						auto intrinsic = video_profile.get_intrinsics();
						profilesPerSensor[i].push_back(profile);
						rgbBuffer_ = cv::Mat(cv::Size(video_profile.width(), video_profile.height()), CV_8UC3, cv::Scalar(0, 0, 0));
						model_ = CameraModel(camera_name, intrinsic.fx, intrinsic.fy, intrinsic.ppx, intrinsic.ppy, this->getLocalTransform(), 0, cv::Size(intrinsic.width, intrinsic.height));
						rgbStreamProfile = profile;
						*rgbIntrinsics_ = intrinsic;
						added = true;
					}
					else if( i==1 // depth
						 && video_profile.format() == RS2_FORMAT_Z16 && video_profile.stream_type() == RS2_STREAM_DEPTH)
					{
						auto intrinsic = video_profile.get_intrinsics();
						profilesPerSensor[i].push_back(profile);
						depthBuffer_ = cv::Mat(cv::Size(video_profile.width(), video_profile.height()), CV_16UC1, cv::Scalar(0));
						depthStreamProfile = profile;
						*depthIntrinsics_ = intrinsic;
						added = true;
					}
				}
				//D400 series:
				else if (!isL500_ &&
						(video_profile.width()  == cameraWidth_ &&
						 video_profile.height() == cameraHeight_ &&
						 video_profile.fps()    == cameraFps_))
				{
					auto intrinsic = video_profile.get_intrinsics();

					// rgb or ir left
					if((!ir_ && video_profile.format() == RS2_FORMAT_RGB8 && video_profile.stream_type() == RS2_STREAM_COLOR) ||
					  (ir_ && video_profile.format() == RS2_FORMAT_Y8 && video_profile.stream_index() == 1))
					{
						if(!profilesPerSensor[i].empty())
						{
							// IR right already there, push ir left front
							profilesPerSensor[i].push_back(profilesPerSensor[i].back());
							profilesPerSensor[i].front() = profile;
						}
						else
						{
							profilesPerSensor[i].push_back(profile);
						}
						rgbBuffer_ = cv::Mat(cv::Size(cameraWidth_, cameraHeight_), video_profile.format() == RS2_FORMAT_Y8?CV_8UC1:CV_8UC3, ir_?cv::Scalar(0):cv::Scalar(0, 0, 0));
						model_ = CameraModel(camera_name, intrinsic.fx, intrinsic.fy, intrinsic.ppx, intrinsic.ppy, this->getLocalTransform(), 0, cv::Size(intrinsic.width, intrinsic.height));
						rgbStreamProfile = profile;
						*rgbIntrinsics_ = intrinsic;
						added = true;
						if(video_profile.format() == RS2_FORMAT_RGB8 || profilesPerSensor[i].size()==2)
						{
							break;
						}
					}
					// depth or ir right
					else if(((!ir_ || irDepth_) && video_profile.format() == RS2_FORMAT_Z16) ||
						   (ir_ && !irDepth_ && video_profile.format() == RS2_FORMAT_Y8 && video_profile.stream_index() == 2))
					{
						profilesPerSensor[i].push_back(profile);
						depthBuffer_ = cv::Mat(cv::Size(cameraWidth_, cameraHeight_), video_profile.format() == RS2_FORMAT_Y8?CV_8UC1:CV_16UC1, cv::Scalar(0));
						depthStreamProfile = profile;
						*depthIntrinsics_ = intrinsic;
						added = true;
						if(!ir_ || irDepth_ || profilesPerSensor[i].size()==2)
						{
							break;
						}
					}
				}
				else if(video_profile.format() == RS2_FORMAT_MOTION_XYZ32F || video_profile.format() == RS2_FORMAT_6DOF)
				{
					//D435i:
					//MOTION_XYZ32F 0 0 200 (gyro)
					//MOTION_XYZ32F 0 0 400 (gyro)
					//MOTION_XYZ32F 0 0 63 6 (accel)
					//MOTION_XYZ32F 0 0 250 6 (accel)
					// or dualMode_ T265:
					//MOTION_XYZ32F 0 0 200 5 (gyro)
					//MOTION_XYZ32F 0 0 62 6 (accel)
					//6DOF 0 0 200 4 (pose)
					bool modified = false;
					for (size_t j= 0; j < profilesPerSensor[i].size(); ++j)
					{
						if (profilesPerSensor[i][j].stream_type() == profile.stream_type())
						{
							if (profile.stream_type() == RS2_STREAM_ACCEL)
							{
								if(profile.fps() > profilesPerSensor[i][j].fps())
									profilesPerSensor[i][j] = profile;
								modified = true;
							}
							else if (profile.stream_type() == RS2_STREAM_GYRO)
							{
								if(profile.fps() < profilesPerSensor[i][j].fps())
									profilesPerSensor[i][j] = profile;
								modified = true;
							}
						}
					}
					if(!modified)
						profilesPerSensor[i].push_back(profile);
					added = true;
				}
			}
			else if(stereo || dualMode_)
			{
				//T265:
				if(!dualMode_ &&
					video_profile.format() == RS2_FORMAT_Y8 &&
					video_profile.width()  == 848 &&
					video_profile.height() == 800 &&
					video_profile.fps()    == 30)
				{
					UASSERT(i<2);
					profilesPerSensor[i].push_back(profile);
					auto intrinsic = video_profile.get_intrinsics();
					if(pi==0)
					{
						// LEFT FISHEYE
						rgbBuffer_ = cv::Mat(cv::Size(848, 800), CV_8UC1, cv::Scalar(0));
						rgbStreamProfile = profile;
						*rgbIntrinsics_ = intrinsic;
					}
					else
					{
						// RIGHT FISHEYE
						depthBuffer_ = cv::Mat(cv::Size(848, 800), CV_8UC1, cv::Scalar(0));
						depthStreamProfile = profile;
						*depthIntrinsics_ = intrinsic;
					}
					added = true;
				}
				else if(video_profile.format() == RS2_FORMAT_MOTION_XYZ32F || video_profile.format() == RS2_FORMAT_6DOF)
				{
					//MOTION_XYZ32F 0 0 200
					//MOTION_XYZ32F 0 0 62
					//6DOF 0 0 200
					profilesPerSensor[i].push_back(profile);
					added = true;
				}
			}
			++pi;
		}
		if (!added)
		{
			UERROR("Given stream configuration is not supported by the device! "
					"Stream Index: %d, Width: %d, Height: %d, FPS: %d", i, cameraWidth_, cameraHeight_, cameraFps_);
			UERROR("Available configurations:");
			for (auto& profile : profiles)
			{
				auto video_profile = profile.as<rs2::video_stream_profile>();
				UERROR("%s %d %d %d %d %s type=%d", rs2_format_to_string(
						video_profile.format()),
						video_profile.width(),
						video_profile.height(),
						video_profile.fps(),
						video_profile.stream_index(),
						video_profile.stream_name().c_str(),
						video_profile.stream_type());
			}
			return false;
		}
	 }
	 UDEBUG("");
	 if(!stereo)
	 {
		 if(!model_.isValidForProjection())
		 {
			 UERROR("Calibration info not valid!");
			 return false;
		 }
		 *depthToRGBExtrinsics_ = depthStreamProfile.get_extrinsics_to(rgbStreamProfile);

		 if(dualMode_)
		 {
			 Transform opticalTransform(0, 0, 1, 0, -1, 0, 0, 0, 0, -1, 0, 0);
			 UINFO("Set base to pose");
			 this->setLocalTransform(this->getLocalTransform()*opticalTransform.inverse());
			 UINFO("poseToLeftIR = %s", dualExtrinsics_.prettyPrint().c_str());
			 Transform baseToCam = this->getLocalTransform()*dualExtrinsics_*opticalTransform;
			 if(!ir_)
			 {
				 Transform leftIRToRGB(
						 depthToRGBExtrinsics_->rotation[0], depthToRGBExtrinsics_->rotation[1], depthToRGBExtrinsics_->rotation[2], depthToRGBExtrinsics_->translation[0],
						 depthToRGBExtrinsics_->rotation[3], depthToRGBExtrinsics_->rotation[4], depthToRGBExtrinsics_->rotation[5], depthToRGBExtrinsics_->translation[1],
						 depthToRGBExtrinsics_->rotation[6], depthToRGBExtrinsics_->rotation[7], depthToRGBExtrinsics_->rotation[8], depthToRGBExtrinsics_->translation[2]);
				 leftIRToRGB = leftIRToRGB.inverse();
				 UINFO("leftIRToRGB = %s", leftIRToRGB.prettyPrint().c_str());
				 baseToCam *= leftIRToRGB;
			 }
			 UASSERT(profilesPerSensor.size()>=2);
			 UASSERT(profilesPerSensor.back().size() == 3);
			 rs2_extrinsics poseToIMU = profilesPerSensor.back()[0].get_extrinsics_to(profilesPerSensor.back()[2]);

			 Transform poseToIMUT(
					 poseToIMU.rotation[0], poseToIMU.rotation[1], poseToIMU.rotation[2], poseToIMU.translation[0],
					 poseToIMU.rotation[3], poseToIMU.rotation[4], poseToIMU.rotation[5], poseToIMU.translation[1],
					 poseToIMU.rotation[6], poseToIMU.rotation[7], poseToIMU.rotation[8], poseToIMU.translation[2]);
			 poseToIMUT = realsense2PoseRotation_ * poseToIMUT;
			 UINFO("poseToIMU = %s", poseToIMUT.prettyPrint().c_str());

			 UINFO("BaseToCam = %s", baseToCam.prettyPrint().c_str());
			 model_.setLocalTransform(baseToCam);
			 imuLocalTransform_ = this->getLocalTransform() * poseToIMUT;
		 }

		 if(ir_ && !irDepth_ && profilesPerSensor.size() >= 2 && profilesPerSensor[1].size() >= 2)
		 {
			 rs2_extrinsics leftToRight = profilesPerSensor[1][1].get_extrinsics_to(profilesPerSensor[1][0]);
			 Transform leftToRightT(
					 leftToRight.rotation[0], leftToRight.rotation[1], leftToRight.rotation[2], leftToRight.translation[0],
					 leftToRight.rotation[3], leftToRight.rotation[4], leftToRight.rotation[5], leftToRight.translation[1],
					 leftToRight.rotation[6], leftToRight.rotation[7], leftToRight.rotation[8], leftToRight.translation[2]);

			 UINFO("left to right transform = %s", leftToRightT.prettyPrint().c_str());

			 // Create stereo camera model from left and right ir of D435
			 stereoModel_ = StereoCameraModel(model_.fx(), model_.fy(), model_.cx(), model_.cy(), leftToRightT.x(), model_.localTransform(), model_.imageSize());
			 UINFO("Stereo parameters: fx=%f cx=%f cy=%f baseline=%f",
						stereoModel_.left().fx(),
						stereoModel_.left().cx(),
						stereoModel_.left().cy(),
						stereoModel_.baseline());
		 }

		 if(!dualMode_ && profilesPerSensor.size() == 3)
		 {
			 if(!profilesPerSensor[2].empty() && !profilesPerSensor[0].empty())
			 {
				 rs2_extrinsics leftToIMU = profilesPerSensor[2][0].get_extrinsics_to(profilesPerSensor[0][0]);
				 Transform leftToIMUT(
						 leftToIMU.rotation[0], leftToIMU.rotation[1], leftToIMU.rotation[2], leftToIMU.translation[0],
						 leftToIMU.rotation[3], leftToIMU.rotation[4], leftToIMU.rotation[5], leftToIMU.translation[1],
						 leftToIMU.rotation[6], leftToIMU.rotation[7], leftToIMU.rotation[8], leftToIMU.translation[2]);
				 imuLocalTransform_ = this->getLocalTransform() * leftToIMUT;
				 UINFO("imu local transform = %s", imuLocalTransform_.prettyPrint().c_str());
			 }
			 else if(!profilesPerSensor[2].empty() && !profilesPerSensor[1].empty())
			 {
				 rs2_extrinsics leftToIMU = profilesPerSensor[2][0].get_extrinsics_to(profilesPerSensor[1][0]);
				 Transform leftToIMUT(
						 leftToIMU.rotation[0], leftToIMU.rotation[1], leftToIMU.rotation[2], leftToIMU.translation[0],
						 leftToIMU.rotation[3], leftToIMU.rotation[4], leftToIMU.rotation[5], leftToIMU.translation[1],
						 leftToIMU.rotation[6], leftToIMU.rotation[7], leftToIMU.rotation[8], leftToIMU.translation[2]);

				 imuLocalTransform_ = this->getLocalTransform() * leftToIMUT;
				 UINFO("imu local transform = %s", imuLocalTransform_.prettyPrint().c_str());
			 }
		 }
	 }
	 else // T265
	 {

		// look for calibration files
		 std::string serial = sn;
		 if(!cameraName.empty())
		 {
			 serial = cameraName;
		 }
		if(!calibrationFolder.empty() && !serial.empty())
		{
			if(!stereoModel_.load(calibrationFolder, serial, false))
			{
				UWARN("Missing calibration files for camera \"%s\" in \"%s\" folder, you should calibrate the camera!",
						serial.c_str(), calibrationFolder.c_str());
			}
			else
			{
				UINFO("Stereo parameters: fx=%f cx=%f cy=%f baseline=%f",
						stereoModel_.left().fx(),
						stereoModel_.left().cx(),
						stereoModel_.left().cy(),
						stereoModel_.baseline());
			}
		}

		// Get extrinsics with pose as the base frame:
		// 0=Left fisheye
		// 1=Right fisheye
		// 2=GYRO
		// 3=ACC
		// 4=POSE
		UASSERT(profilesPerSensor[0].size() == 5);
		if(odometryProvided_)
		{
			rs2_extrinsics poseToLeft = profilesPerSensor[0][0].get_extrinsics_to(profilesPerSensor[0][4]);
			rs2_extrinsics poseToIMU = profilesPerSensor[0][2].get_extrinsics_to(profilesPerSensor[0][4]);
			Transform poseToLeftT(
					poseToLeft.rotation[0], poseToLeft.rotation[1], poseToLeft.rotation[2], poseToLeft.translation[0],
					poseToLeft.rotation[3], poseToLeft.rotation[4], poseToLeft.rotation[5], poseToLeft.translation[1],
					poseToLeft.rotation[6], poseToLeft.rotation[7], poseToLeft.rotation[8], poseToLeft.translation[2]);
			poseToLeftT = realsense2PoseRotation_ * poseToLeftT;
			UINFO("poseToLeft = %s", poseToLeftT.prettyPrint().c_str());

			Transform poseToIMUT(
					poseToIMU.rotation[0], poseToIMU.rotation[1], poseToIMU.rotation[2], poseToIMU.translation[0],
					poseToIMU.rotation[3], poseToIMU.rotation[4], poseToIMU.rotation[5], poseToIMU.translation[1],
					poseToIMU.rotation[6], poseToIMU.rotation[7], poseToIMU.rotation[8], poseToIMU.translation[2]);
			poseToIMUT = realsense2PoseRotation_ * poseToIMUT;
			UINFO("poseToIMU = %s", poseToIMUT.prettyPrint().c_str());

			UINFO("Set base to pose");
			Transform opticalTransform(0, 0, 1, 0, -1, 0, 0, 0, 0, -1, 0, 0);
			this->setLocalTransform(this->getLocalTransform() * opticalTransform.inverse());
			stereoModel_.setLocalTransform(this->getLocalTransform()*poseToLeftT);
			imuLocalTransform_ = this->getLocalTransform()* poseToIMUT;
		}
		else
		{
			// Set imu transform based on the left camera instead of pose
			 rs2_extrinsics leftToIMU = profilesPerSensor[0][2].get_extrinsics_to(profilesPerSensor[0][0]);
			 Transform leftToIMUT(
					 leftToIMU.rotation[0], leftToIMU.rotation[1], leftToIMU.rotation[2], leftToIMU.translation[0],
					 leftToIMU.rotation[3], leftToIMU.rotation[4], leftToIMU.rotation[5], leftToIMU.translation[1],
					 leftToIMU.rotation[6], leftToIMU.rotation[7], leftToIMU.rotation[8], leftToIMU.translation[2]);
			 UINFO("leftToIMU = %s", leftToIMUT.prettyPrint().c_str());
			 imuLocalTransform_ = this->getLocalTransform() * leftToIMUT;
			 UINFO("imu local transform = %s", imuLocalTransform_.prettyPrint().c_str());
			 stereoModel_.setLocalTransform(this->getLocalTransform());
		}
		if(rectifyImages_ && !stereoModel_.isValidForRectification())
		{
			UERROR("Parameter \"rectifyImages\" is set, but no stereo model is loaded or valid.");
			return false;
		}
	 }

	 std::function<void(rs2::frame)> multiple_message_callback_function = [this](rs2::frame frame){multiple_message_callback(frame);};

	 for (unsigned int i=0; i<sensors.size(); ++i)
	 {
		 if(profilesPerSensor[i].size())
		 {
			 UINFO("Starting sensor %d with %d profiles", (int)i, (int)profilesPerSensor[i].size());
			 for (size_t j = 0; j < profilesPerSensor[i].size(); ++j)
			 {
				 auto video_profile = profilesPerSensor[i][j].as<rs2::video_stream_profile>();
				 UINFO("Opening: %s %d %d %d %d %s type=%d", rs2_format_to_string(
						 video_profile.format()),
						 video_profile.width(),
						 video_profile.height(),
						 video_profile.fps(),
						 video_profile.stream_index(),
						 video_profile.stream_name().c_str(),
						 video_profile.stream_type());
			 }
			 if(globalTimeSync_ && sensors[i].supports(rs2_option::RS2_OPTION_GLOBAL_TIME_ENABLED))
			 {
				 float value = sensors[i].get_option(rs2_option::RS2_OPTION_GLOBAL_TIME_ENABLED);
				 UINFO("Set RS2_OPTION_GLOBAL_TIME_ENABLED=1 (was %f) for sensor %d", value, (int)i);
				 sensors[i].set_option(rs2_option::RS2_OPTION_GLOBAL_TIME_ENABLED, 1);
			 }
			 sensors[i].open(profilesPerSensor[i]);
			 if(sensors[i].is<rs2::depth_sensor>())
			 {
				 auto depth_sensor = sensors[i].as<rs2::depth_sensor>();
				 depth_scale_meters_ = depth_sensor.get_depth_scale();
				 UINFO("Depth scale %f for sensor %d", depth_scale_meters_, (int)i);
			 }
			 sensors[i].start(multiple_message_callback_function);
		 }
	 }

	 uSleep(1000); // ignore the first frames
	 UINFO("Enabling streams...done!");

	return true;

#else
	UERROR("CameraRealSense: RTAB-Map is not built with RealSense2 support!");
	return false;
#endif
}

bool CameraRealSense2::isCalibrated() const
{
#ifdef RTABMAP_REALSENSE2
	return model_.isValidForProjection() || stereoModel_.isValidForRectification();
#else
	return false;
#endif
}

std::string CameraRealSense2::getSerial() const
{
#ifdef RTABMAP_REALSENSE2
	if(dev_[0])
	{
		return dev_[0]->get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
	}
#endif
	return "NA";
}

bool CameraRealSense2::odomProvided() const
{
#ifdef RTABMAP_REALSENSE2
	return odometryProvided_;
#else
	return false;
#endif
}

void CameraRealSense2::setEmitterEnabled(bool enabled)
{
#ifdef RTABMAP_REALSENSE2
	emitterEnabled_ = enabled;
#endif
}

void CameraRealSense2::setIRFormat(bool enabled, bool useDepthInsteadOfRightImage)
{
#ifdef RTABMAP_REALSENSE2
	ir_ = enabled;
	irDepth_ = useDepthInsteadOfRightImage;
#endif
}

void CameraRealSense2::setResolution(int width, int height, int fps)
{
#ifdef RTABMAP_REALSENSE2
	cameraWidth_ = width;
	cameraHeight_ = height;
	cameraFps_ = fps;
#endif
}

void CameraRealSense2::setGlobalTimeSync(bool enabled)
{
#ifdef RTABMAP_REALSENSE2
	globalTimeSync_ = enabled;
#endif
}

void CameraRealSense2::publishInterIMU(bool enabled)
{
#ifdef RTABMAP_REALSENSE2
	publishInterIMU_ = enabled;
#endif
}

void CameraRealSense2::setDualMode(bool enabled, const Transform & extrinsics)
{
#ifdef RTABMAP_REALSENSE2
	UASSERT(!enabled || !extrinsics.isNull());
	dualMode_ = enabled;
	dualExtrinsics_ =  extrinsics;
	if(dualMode_)
	{
		odometryProvided_ = true;
	}
#endif
}

void CameraRealSense2::setJsonConfig(const std::string & json)
{
#ifdef RTABMAP_REALSENSE2
	jsonConfig_ = json;
#endif
}

void CameraRealSense2::setImagesRectified(bool enabled)
{
#ifdef RTABMAP_REALSENSE2
	rectifyImages_ = enabled;
#endif
}

void CameraRealSense2::setOdomProvided(bool enabled)
{
#ifdef RTABMAP_REALSENSE2
	if(dualMode_ && !enabled)
	{
		UERROR("Odometry is disabled but dual mode was enabled, disabling dual mode.");
		dualMode_ = false;
	}
	odometryProvided_ = enabled;
#endif
}

SensorData CameraRealSense2::captureImage(CameraInfo * info)
{
	SensorData data;
#ifdef RTABMAP_REALSENSE2

	try{
		auto frameset = syncer_->wait_for_frames(5000);
		UTimer timer;
		int desiredFramesetSize = 2;
		if(isL500_ && globalTimeSync_)
			desiredFramesetSize = 3;
		while ((int)frameset.size() != desiredFramesetSize && timer.elapsed() < 2.0)
		{
			// maybe there is a latency with the USB, try again in 100 ms (for the next 2 seconds)
			frameset = syncer_->wait_for_frames(100);
		}
		if ((int)frameset.size() == desiredFramesetSize)
		{
			double now = UTimer::now();
			bool is_rgb_arrived = false;
			bool is_depth_arrived = false;
			bool is_left_fisheye_arrived = false;
			bool is_right_fisheye_arrived = false;
			rs2::frame rgb_frame;
			rs2::frame depth_frame;
			double stamp = frameset.get_timestamp();
			for (auto it = frameset.begin(); it != frameset.end(); ++it)
			{
				auto f = (*it);
				if(stamp > f.get_timestamp())
				{
					stamp = f.get_timestamp();
				}
				auto stream_type = f.get_profile().stream_type();
				if (stream_type == RS2_STREAM_COLOR || stream_type == RS2_STREAM_INFRARED)
				{
					if(isL500_)
					{
						if(stream_type == RS2_STREAM_COLOR)
						{
							rgb_frame = f;
							is_rgb_arrived = true;
						}
					} 
					else if(ir_ && !irDepth_)
					{
						//stereo D435
						if(!is_depth_arrived)
						{
							depth_frame = f; // right image
							is_depth_arrived = true;
						}
						else
						{
							rgb_frame = f; // left image
							is_rgb_arrived = true;
						}
					}
					else
					{
						rgb_frame = f;
						is_rgb_arrived = true;
					}
				}
				else if (stream_type == RS2_STREAM_DEPTH)
				{
					depth_frame = f;
					is_depth_arrived = true;
				}
				else if (stream_type == RS2_STREAM_FISHEYE)
				{
					if(!is_right_fisheye_arrived)
					{
						depth_frame = f;
						is_right_fisheye_arrived = true;
					}
					else
					{
						rgb_frame = f;
						is_left_fisheye_arrived = true;
					}
				}
			}

			stamp /= 1000.0; // put in seconds
			UDEBUG("Frameset arrived. system=%fs frame=%fs", now, stamp);
			if(stamp - now > 1000000000.0)
			{
				if(!clockSyncWarningShown_)
				{
					UWARN("Clocks are not sync with host computer! Detected stamps in far "
							"future %f, thus using host time instead (%f)! This message "
							"will only appear once. "
							"See https://github.com/IntelRealSense/librealsense/issues/4505 "
							"for more info", stamp, now);
					clockSyncWarningShown_ = true;
				}
				stamp = now;
			}

			if(is_rgb_arrived && is_depth_arrived)
			{
				cv::Mat depth;
				if(ir_)
				{
					depth = cv::Mat(depthBuffer_.size(), depthBuffer_.type(), (void*)depth_frame.get_data()).clone();
				}
				else
				{
					rs2::align align(rgb_frame.get_profile().stream_type());
					rs2::frameset processed = frameset.apply_filter(align);
					rs2::depth_frame aligned_depth_frame = processed.get_depth_frame();
					depth = cv::Mat(depthBuffer_.size(), depthBuffer_.type(), (void*)aligned_depth_frame.get_data()).clone();
					if(depth_scale_meters_ != 0.001f)
					{ // convert to mm
						if(depth.type() ==  CV_16UC1)
						{
							float scale = depth_scale_meters_ / 0.001f;
							uint16_t *p = depth.ptr<uint16_t>();
							int buffSize = depth.rows * depth.cols;
							#pragma omp parallel for
							for(int i = 0; i < buffSize; ++i) {
								p[i] *= scale;
							}
						}
					}
				}

				cv::Mat rgb = cv::Mat(rgbBuffer_.size(), rgbBuffer_.type(), (void*)rgb_frame.get_data());
				cv::Mat bgr;
				if(rgb.channels() == 3)
				{
					cv::cvtColor(rgb, bgr, CV_RGB2BGR);
				}
				else
				{
					bgr = rgb.clone();
				}

				if(ir_ && !irDepth_)
				{
					//stereo D435
					data = SensorData(bgr, depth, stereoModel_, this->getNextSeqID(), stamp);
				}
				else
				{
					data = SensorData(bgr, depth, model_, this->getNextSeqID(), stamp);
				}
			}
			else if(is_left_fisheye_arrived && is_right_fisheye_arrived)
			{
				auto from_image_frame = depth_frame.as<rs2::video_frame>();
				cv::Mat left,right;
				if(rectifyImages_ && stereoModel_.left().isValidForRectification() && stereoModel_.right().isValidForRectification())
				{
					left = stereoModel_.left().rectifyImage(cv::Mat(rgbBuffer_.size(), rgbBuffer_.type(), (void*)rgb_frame.get_data()));
					right = stereoModel_.right().rectifyImage(cv::Mat(depthBuffer_.size(), depthBuffer_.type(), (void*)depth_frame.get_data()));
				}
				else
				{
					left = cv::Mat(rgbBuffer_.size(), rgbBuffer_.type(), (void*)rgb_frame.get_data()).clone();
					right = cv::Mat(depthBuffer_.size(), depthBuffer_.type(), (void*)depth_frame.get_data()).clone();
				}

				if(stereoModel_.left().imageHeight() == 0 || stereoModel_.left().imageWidth() == 0)
				{
					stereoModel_.setImageSize(left.size());
				}

				data = SensorData(left, right, stereoModel_, this->getNextSeqID(), stamp);
			}
			else
			{
				UERROR("Not received depth and rgb");
			}

			IMU imu;
			unsigned int confidence = 0;
			double imuStamp = stamp*1000.0;
			Transform pose;
			getPoseAndIMU(imuStamp, pose, confidence, imu);

			if(info && odometryProvided_ && !pose.isNull())
			{
				// Transform in base frame (local transform should contain base to pose transform)
				info->odomPose = this->getLocalTransform() * pose * this->getLocalTransform().inverse();

				info->odomCovariance = cv::Mat::eye(6,6,CV_64FC1) * 0.0001;
				info->odomCovariance.rowRange(0,3) *= pow(10, 3-(int)confidence);
				info->odomCovariance.rowRange(3,6) *= pow(10, 1-(int)confidence);
			}
			if(!imu.empty() && !publishInterIMU_)
			{
				data.setIMU(imu);
			}
			else if(publishInterIMU_ && !gyroBuffer_.empty())
			{
				if(lastImuStamp_ > 0.0)
				{
					UASSERT(imuStamp > lastImuStamp_);
					imuMutex_.lock();
					std::map<double, cv::Vec3f>::iterator iterA = gyroBuffer_.upper_bound(lastImuStamp_);
					std::map<double, cv::Vec3f>::iterator iterB = gyroBuffer_.lower_bound(imuStamp);
					if(iterA != gyroBuffer_.end())
					{
						++iterA;
					}
					if(iterB != gyroBuffer_.end())
					{
						++iterB;
					}
					if(iterA != iterB)
					{
						int pub = 0;
						for(;iterA != iterB;++iterA)
						{
							Transform tmp;
							IMU imuTmp;
							getPoseAndIMU(iterA->first, tmp, confidence, imuTmp);
							if(!imuTmp.empty())
							{
								UEventsManager::post(new IMUEvent(imuTmp, iterA->first/1000.0));
								pub++;
							}
							else
							{
								break;
							}
						}
						UDEBUG("inter imu published=%d, %f -> %f", pub, lastImuStamp_, imuStamp);
					}
					imuMutex_.unlock();
				}
				lastImuStamp_ = imuStamp;
			}
		}
		else
		{
			UERROR("Missing frames (received %d)", (int)frameset.size());
		}
	}
	catch(const std::exception& ex)
	{
		UERROR("An error has occurred during frame callback: %s", ex.what());
	}
#else
	UERROR("CameraRealSense2: RTAB-Map is not built with RealSense2 support!");
#endif
	return data;
}

} // namespace rtabmap
