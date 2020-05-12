/****************************************************************************
*
*   Copyright (c) 2016-2020 PX4 Development Team. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
*
* 1. Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in
*    the documentation and/or other materials provided with the
*    distribution.
* 3. Neither the name PX4 nor the names of its contributors may be
*    used to endorse or promote products derived from this software
*    without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
* ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*
****************************************************************************/

/**
 * @file input_mavlink.cpp
 * @author Leon Müller (thedevleon)
 * @author Beat Küng <beat-kueng@gmx.net>
 *
 */

#include "input_mavlink.h"
#include <uORB/Publication.hpp>
#include <uORB/topics/gimbal_device_information.h>
#include <uORB/topics/vehicle_roi.h>
#include <uORB/topics/vehicle_command_ack.h>
#include <uORB/topics/position_setpoint_triplet.h>
#include <drivers/drv_hrt.h>
#include <lib/parameters/param.h>
#include <px4_platform_common/defines.h>
#include <px4_platform_common/posix.h>
#include <errno.h>
#include <math.h>
#include <matrix/matrix/math.hpp>

using matrix::wrap_pi;

namespace vmount
{

InputMavlinkROI::~InputMavlinkROI()
{
	if (_vehicle_roi_sub >= 0) {
		orb_unsubscribe(_vehicle_roi_sub);
	}

	if (_position_setpoint_triplet_sub >= 0) {
		orb_unsubscribe(_position_setpoint_triplet_sub);
	}
}

int InputMavlinkROI::initialize()
{
	_vehicle_roi_sub = orb_subscribe(ORB_ID(vehicle_roi));

	if (_vehicle_roi_sub < 0) {
		return -errno;
	}

	_position_setpoint_triplet_sub = orb_subscribe(ORB_ID(position_setpoint_triplet));

	if (_position_setpoint_triplet_sub < 0) {
		return -errno;
	}

	return 0;
}

int InputMavlinkROI::update_impl(unsigned int timeout_ms, ControlData **control_data, bool already_active)
{
	// already_active is unused, we don't care what happened previously.

	// Default to no change, set if we receive anything.
	*control_data = nullptr;

	const int num_poll = 2;
	px4_pollfd_struct_t polls[num_poll];
	polls[0].fd = 		_vehicle_roi_sub;
	polls[0].events = 	POLLIN;
	polls[1].fd = 		_position_setpoint_triplet_sub;
	polls[1].events = 	POLLIN;

	int ret = px4_poll(polls, num_poll, timeout_ms);

	if (ret < 0) {
		return -errno;
	}

	if (ret == 0) {
		// Timeout, _control_data is already null

	} else {
		if (polls[0].revents & POLLIN) {
			vehicle_roi_s vehicle_roi;
			orb_copy(ORB_ID(vehicle_roi), _vehicle_roi_sub, &vehicle_roi);

			_control_data.gimbal_shutter_retract = false;

			if (vehicle_roi.mode == vehicle_roi_s::ROI_NONE) {

				_control_data.type = ControlData::Type::Neutral;
				*control_data = &_control_data;

			} else if (vehicle_roi.mode == vehicle_roi_s::ROI_WPNEXT) {
				_control_data.type = ControlData::Type::LonLat;
				_read_control_data_from_position_setpoint_sub();
				_control_data.type_data.lonlat.pitch_fixed_angle = -10.f;

				_control_data.type_data.lonlat.roll_angle = vehicle_roi.roll_offset;
				_control_data.type_data.lonlat.pitch_angle_offset = vehicle_roi.pitch_offset;
				_control_data.type_data.lonlat.yaw_angle_offset = vehicle_roi.yaw_offset;

				*control_data = &_control_data;

			} else if (vehicle_roi.mode == vehicle_roi_s::ROI_LOCATION) {
				control_data_set_lon_lat(vehicle_roi.lon, vehicle_roi.lat, vehicle_roi.alt);

				*control_data = &_control_data;

			} else if (vehicle_roi.mode == vehicle_roi_s::ROI_TARGET) {
				//TODO is this even suported?
			}

			_cur_roi_mode = vehicle_roi.mode;
		}

		// check whether the position setpoint got updated
		if (polls[1].revents & POLLIN) {
			if (_cur_roi_mode == vehicle_roi_s::ROI_WPNEXT) {
				_read_control_data_from_position_setpoint_sub();
				*control_data = &_control_data;

			} else { // must do an orb_copy() in *every* case
				position_setpoint_triplet_s position_setpoint_triplet;
				orb_copy(ORB_ID(position_setpoint_triplet), _position_setpoint_triplet_sub, &position_setpoint_triplet);
			}
		}
	}

	return 0;
}

void InputMavlinkROI::_read_control_data_from_position_setpoint_sub()
{
	position_setpoint_triplet_s position_setpoint_triplet;
	orb_copy(ORB_ID(position_setpoint_triplet), _position_setpoint_triplet_sub, &position_setpoint_triplet);
	_control_data.type_data.lonlat.lon = position_setpoint_triplet.current.lon;
	_control_data.type_data.lonlat.lat = position_setpoint_triplet.current.lat;
	_control_data.type_data.lonlat.altitude = position_setpoint_triplet.current.alt;
}

void InputMavlinkROI::print_status()
{
	PX4_INFO("Input: Mavlink (ROI)");
}


InputMavlinkCmdMount::InputMavlinkCmdMount()
{
	param_t handle = param_find("MAV_SYS_ID");

	if (handle != PARAM_INVALID) {
		param_get(handle, &_mav_sys_id);
	}

	handle = param_find("MAV_COMP_ID");

	if (handle != PARAM_INVALID) {
		param_get(handle, &_mav_comp_id);
	}
}

InputMavlinkCmdMount::~InputMavlinkCmdMount()
{
	if (_vehicle_command_sub >= 0) {
		orb_unsubscribe(_vehicle_command_sub);
	}
}

int InputMavlinkCmdMount::initialize()
{
	if ((_vehicle_command_sub = orb_subscribe(ORB_ID(vehicle_command))) < 0) {
		return -errno;
	}

	// rate-limit inputs to 100Hz. If we don't do this and the output is configured to mavlink mode,
	// it will publish vehicle_command's as well, causing the input poll() in here to return
	// immediately, which in turn will cause an output update and thus a busy loop.
	orb_set_interval(_vehicle_command_sub, 10);

	return 0;
}


int InputMavlinkCmdMount::update_impl(unsigned int timeout_ms, ControlData **control_data, bool already_active)
{
	// Default to notify that there was no change.
	*control_data = nullptr;

	const int num_poll = 1;
	px4_pollfd_struct_t polls[num_poll];
	polls[0].fd = 		_vehicle_command_sub;
	polls[0].events = 	POLLIN;

	int poll_timeout = (int)timeout_ms;

	bool exit_loop = false;

	while (!exit_loop && poll_timeout >= 0) {
		hrt_abstime poll_start = hrt_absolute_time();

		int ret = px4_poll(polls, num_poll, poll_timeout);

		if (ret < 0) {
			return -errno;
		}

		poll_timeout -= (hrt_absolute_time() - poll_start) / 1000;

		// if we get a command that we need to handle, we exit the loop, otherwise we poll until we reach the timeout
		exit_loop = true;

		if (ret == 0) {
			// Timeout control_data already null.

		} else {
			if (polls[0].revents & POLLIN) {
				vehicle_command_s vehicle_command;
				orb_copy(ORB_ID(vehicle_command), _vehicle_command_sub, &vehicle_command);

				// Process only if the command is for us or for anyone (component id 0).
				const bool sysid_correct = (vehicle_command.target_system == _mav_sys_id);
				const bool compid_correct = ((vehicle_command.target_component == _mav_comp_id) ||
							     (vehicle_command.target_component == 0));

				if (!sysid_correct || !compid_correct) {
					exit_loop = false;
					continue;
				}

				_control_data.gimbal_shutter_retract = false;

				if (vehicle_command.command == vehicle_command_s::VEHICLE_CMD_DO_MOUNT_CONTROL) {

					switch ((int)vehicle_command.param7) {
					case vehicle_command_s::VEHICLE_MOUNT_MODE_RETRACT:
						_control_data.gimbal_shutter_retract = true;

					/* FALLTHROUGH */

					case vehicle_command_s::VEHICLE_MOUNT_MODE_NEUTRAL:
						_control_data.type = ControlData::Type::Neutral;

						*control_data = &_control_data;
						break;

					case vehicle_command_s::VEHICLE_MOUNT_MODE_MAVLINK_TARGETING:
						_control_data.type = ControlData::Type::Angle;
						_control_data.type_data.angle.frames[0] = ControlData::TypeData::TypeAngle::Frame::AngleBodyFrame;
						_control_data.type_data.angle.frames[1] = ControlData::TypeData::TypeAngle::Frame::AngleBodyFrame;
						_control_data.type_data.angle.frames[2] = ControlData::TypeData::TypeAngle::Frame::AngleBodyFrame;
						// vmount spec has roll on channel 0, MAVLink spec has pitch on channel 0
						_control_data.type_data.angle.angles[0] = vehicle_command.param2 * M_DEG_TO_RAD_F;
						// vmount spec has pitch on channel 1, MAVLink spec has roll on channel 1
						_control_data.type_data.angle.angles[1] = vehicle_command.param1 * M_DEG_TO_RAD_F;
						// both specs have yaw on channel 2
						_control_data.type_data.angle.angles[2] = vehicle_command.param3 * M_DEG_TO_RAD_F;

						// We expect angle of [-pi..+pi]. If the input range is [0..2pi] we can fix that.
						if (_control_data.type_data.angle.angles[2] > M_PI_F) {
							_control_data.type_data.angle.angles[2] -= 2 * M_PI_F;
						}

						*control_data = &_control_data;
						break;

					case vehicle_command_s::VEHICLE_MOUNT_MODE_RC_TARGETING:
						break;

					case vehicle_command_s::VEHICLE_MOUNT_MODE_GPS_POINT:
						control_data_set_lon_lat((double)vehicle_command.param6, (double)vehicle_command.param5, vehicle_command.param4);

						*control_data = &_control_data;
						break;
					}

					_ack_vehicle_command(&vehicle_command);

				} else if (vehicle_command.command == vehicle_command_s::VEHICLE_CMD_DO_MOUNT_CONFIGURE) {

					_control_data.stabilize_axis[0] = (int)(vehicle_command.param2 + 0.5f) == 1;
					_control_data.stabilize_axis[1] = (int)(vehicle_command.param3 + 0.5f) == 1;
					_control_data.stabilize_axis[2] = (int)(vehicle_command.param4 + 0.5f) == 1;


					const int params[] = {
						(int)((float)vehicle_command.param5 + 0.5f),
						(int)((float)vehicle_command.param6 + 0.5f),
						(int)(vehicle_command.param7 + 0.5f)
					};

					for (int i = 0; i < 3; ++i) {

						if (params[i] == 0) {
							_control_data.type_data.angle.frames[i] =
								ControlData::TypeData::TypeAngle::Frame::AngleBodyFrame;

						} else if (params[i] == 1) {
							_control_data.type_data.angle.frames[i] =
								ControlData::TypeData::TypeAngle::Frame::AngularRate;

						} else if (params[i] == 2) {
							_control_data.type_data.angle.frames[i] =
								ControlData::TypeData::TypeAngle::Frame::AngleAbsoluteFrame;

						} else {
							// Not supported, fallback to body angle.
							_control_data.type_data.angle.frames[i] =
								ControlData::TypeData::TypeAngle::Frame::AngleBodyFrame;
						}
					}

					_control_data.type = ControlData::Type::Neutral; //always switch to neutral position

					*control_data = &_control_data;
					_ack_vehicle_command(&vehicle_command);

				} else {
					exit_loop = false;
				}
			}

		}
	}

	return 0;
}

void InputMavlinkCmdMount::_ack_vehicle_command(vehicle_command_s *cmd)
{
	vehicle_command_ack_s vehicle_command_ack{};

	vehicle_command_ack.timestamp = hrt_absolute_time();
	vehicle_command_ack.command = cmd->command;
	vehicle_command_ack.result = vehicle_command_s::VEHICLE_CMD_RESULT_ACCEPTED;
	vehicle_command_ack.target_system = cmd->source_system;
	vehicle_command_ack.target_component = cmd->source_component;

	uORB::Publication<vehicle_command_ack_s> cmd_ack_pub{ORB_ID(vehicle_command_ack)};
	cmd_ack_pub.publish(vehicle_command_ack);
}

void InputMavlinkCmdMount::print_status()
{
	PX4_INFO("Input: Mavlink (CMD_MOUNT)");
}

InputMavlinkGimbalV2::InputMavlinkGimbalV2(bool has_v2_gimbal_device)
{
	param_t handle = param_find("MAV_SYS_ID");

	if (handle != PARAM_INVALID) {
		param_get(handle, &_mav_sys_id);
	}

	handle = param_find("MAV_COMP_ID");

	if (handle != PARAM_INVALID) {
		param_get(handle, &_mav_comp_id);
	}

	if (has_v2_gimbal_device) {
		/* smart gimbal: ask GIMBAL_DEVICE_INFORMATION to it */
		_request_gimbal_device_information();

	} else {
		/* dumb gimbal or MAVLink v1 protocol gimbal: fake GIMBAL_DEVICE_INFORMATION */
		_stream_gimbal_manager_information();
	}
}

InputMavlinkGimbalV2::~InputMavlinkGimbalV2()
{
	if (_vehicle_roi_sub >= 0) {
		orb_unsubscribe(_vehicle_roi_sub);
	}

	if (_position_setpoint_triplet_sub >= 0) {
		orb_unsubscribe(_position_setpoint_triplet_sub);
	}

	if (_gimbal_manager_set_attitude_sub >= 0) {
		orb_unsubscribe(_gimbal_manager_set_attitude_sub);
	}

	if (_vehicle_command_sub >= 0) {
		orb_unsubscribe(_vehicle_command_sub);
	}
}


void InputMavlinkGimbalV2::print_status()
{
	PX4_INFO("Input: Mavlink (Gimbal V2)");
}

int InputMavlinkGimbalV2::initialize()
{
	_vehicle_roi_sub = orb_subscribe(ORB_ID(vehicle_roi));

	if (_vehicle_roi_sub < 0) {
		return -errno;
	}

	_position_setpoint_triplet_sub = orb_subscribe(ORB_ID(position_setpoint_triplet));

	if (_position_setpoint_triplet_sub < 0) {
		return -errno;
	}

	_gimbal_manager_set_attitude_sub  = orb_subscribe(ORB_ID(gimbal_manager_set_attitude));

	if (_gimbal_manager_set_attitude_sub < 0) {
		return -errno;
	}

	if ((_vehicle_command_sub = orb_subscribe(ORB_ID(vehicle_command))) < 0) {
		return -errno;
	}

	// rate-limit inputs to 100Hz. If we don't do this and the output is configured to mavlink mode,
	// it will publish vehicle_command's as well, causing the input poll() in here to return
	// immediately, which in turn will cause an output update and thus a busy loop.
	orb_set_interval(_vehicle_command_sub, 10);

	return 0;
}

void InputMavlinkGimbalV2::_stream_gimbal_manager_status()
{

	if (_gimbal_device_attitude_status_sub.updated()) {
		_gimbal_device_attitude_status_sub.copy(&_gimbal_device_attitude_status);
	}

	gimbal_manager_status_s gimbal_manager_status{};
	gimbal_manager_status.timestamp = hrt_absolute_time();
	gimbal_manager_status.flags = _gimbal_device_attitude_status.device_flags;
	gimbal_manager_status.gimbal_device_id = 0;
	_gimbal_manager_status_pub.publish(gimbal_manager_status);

}

void InputMavlinkGimbalV2::_stream_gimbal_manager_information()
{
	gimbal_device_information_s gimbal_device_info;
	gimbal_device_info.timestamp = hrt_absolute_time();
	const char vendor_name[] = "PX4";
	const char model_name[] = "AUX gimbal";

	strncpy((char *)gimbal_device_info.vendor_name, vendor_name, sizeof(gimbal_device_info.vendor_name));
	strncpy((char *)gimbal_device_info.model_name, model_name, sizeof(gimbal_device_info.model_name));

	gimbal_device_info.firmware_version = 0;
	gimbal_device_info.capability_flags = 0;
	gimbal_device_info.capability_flags = gimbal_device_information_s::GIMBAL_DEVICE_CAP_FLAGS_HAS_NEUTRAL |
					      gimbal_device_information_s::GIMBAL_DEVICE_CAP_FLAGS_HAS_ROLL_LOCK |
					      gimbal_device_information_s::GIMBAL_DEVICE_CAP_FLAGS_HAS_PITCH_AXIS |
					      gimbal_device_information_s::GIMBAL_DEVICE_CAP_FLAGS_HAS_PITCH_LOCK |
					      gimbal_device_information_s::GIMBAL_DEVICE_CAP_FLAGS_HAS_YAW_AXIS |
					      gimbal_device_information_s::GIMBAL_DEVICE_CAP_FLAGS_HAS_YAW_LOCK;

	gimbal_device_info.tilt_max = M_PI_F / 2;
	gimbal_device_info.tilt_min = -M_PI_F / 2;
	gimbal_device_info.tilt_rate_max = 1;
	gimbal_device_info.pan_max = M_PI_F;
	gimbal_device_info.pan_min = -M_PI_F;
	gimbal_device_info.pan_rate_max = 1;

	_gimbal_device_info_pub.publish(gimbal_device_info);
}

void InputMavlinkGimbalV2::_request_gimbal_device_information()
{
	vehicle_command_s vehicle_cmd{};
	vehicle_cmd.timestamp = hrt_absolute_time();
	vehicle_cmd.command = vehicle_command_s::VEHICLE_CMD_REQUEST_MESSAGE;
	vehicle_cmd.param1 = vehicle_command_s::VEHICLE_CMD_GIMBAL_DEVICE_INFORMATION;
	vehicle_cmd.target_system = 0;
	vehicle_cmd.target_component = 0;
	vehicle_cmd.source_system = _mav_sys_id;
	vehicle_cmd.source_component = _mav_comp_id;
	vehicle_cmd.confirmation = 0;
	vehicle_cmd.from_external = false;

	uORB::PublicationQueued<vehicle_command_s> vehicle_command_pub{ORB_ID(vehicle_command)};
	vehicle_command_pub.publish(vehicle_cmd);
}

int InputMavlinkGimbalV2::update_impl(unsigned int timeout_ms, ControlData **control_data, bool already_active)
{
	_stream_gimbal_manager_status();

	// Default to no change, set if we receive anything.
	*control_data = nullptr;

	const int num_poll = 4;
	px4_pollfd_struct_t polls[num_poll];
	polls[0].fd = 		_gimbal_manager_set_attitude_sub;
	polls[0].events = 	POLLIN;
	polls[1].fd = 		_vehicle_roi_sub;
	polls[1].events = 	POLLIN;
	polls[2].fd = 		_position_setpoint_triplet_sub;
	polls[2].events = 	POLLIN;
	polls[3].fd = 		_vehicle_command_sub;
	polls[3].events = 	POLLIN;

	int poll_timeout = (int)timeout_ms;

	bool exit_loop = false;

	while (!exit_loop && poll_timeout >= 0) {
		hrt_abstime poll_start = hrt_absolute_time();

		int ret = px4_poll(polls, num_poll, poll_timeout);

		if (ret < 0) {
			return -errno;
		}

		poll_timeout -= (hrt_absolute_time() - poll_start) / 1000;

		// if we get a command that we need to handle, we exit the loop, otherwise we poll until we reach the timeout
		exit_loop = true;

		if (ret == 0) {
			// Timeout control_data already null.

		} else {
			if (polls[0].revents & POLLIN) {
				gimbal_manager_set_attitude_s set_attitude;
				orb_copy(ORB_ID(gimbal_manager_set_attitude), _gimbal_manager_set_attitude_sub, &set_attitude);

				const float pitch = matrix::Eulerf(matrix::Quatf(set_attitude.q)).phi(); // rad
				const float roll = matrix::Eulerf(matrix::Quatf(set_attitude.q)).theta();
				const float yaw = matrix::Eulerf(matrix::Quatf(set_attitude.q)).psi();

				_set_control_data_from_set_attitude(set_attitude.flags, pitch, set_attitude.angular_velocity_y, yaw,
								    set_attitude.angular_velocity_z, roll, set_attitude.angular_velocity_x);
				*control_data = &_control_data;
			}

			if (polls[1].revents & POLLIN) {
				vehicle_roi_s vehicle_roi;
				orb_copy(ORB_ID(vehicle_roi), _vehicle_roi_sub, &vehicle_roi);

				_control_data.gimbal_shutter_retract = false;

				if (vehicle_roi.mode == vehicle_roi_s::ROI_NONE) {

					_control_data.type = ControlData::Type::Neutral;
					*control_data = &_control_data;
					_is_roi_set = false;
					_cur_roi_mode = vehicle_roi.mode;

				} else if (vehicle_roi.mode == vehicle_roi_s::ROI_WPNEXT) {
					double lat, lon, alt = 0.;
					_read_lat_lon_alt_from_position_setpoint_sub(lon, lat, alt);
					_control_data.type_data.lonlat.pitch_fixed_angle = -10.f;

					_control_data.type_data.lonlat.roll_angle = vehicle_roi.roll_offset;
					_control_data.type_data.lonlat.pitch_angle_offset = vehicle_roi.pitch_offset;
					_control_data.type_data.lonlat.yaw_angle_offset = vehicle_roi.yaw_offset;

					_transform_lon_lat_to_angle(lon, lat, alt);

					*control_data = &_control_data;
					_is_roi_set = true;
					_cur_roi_mode = vehicle_roi.mode;

				} else if (vehicle_roi.mode == vehicle_roi_s::ROI_LOCATION) {

					_transform_lon_lat_to_angle(vehicle_roi.lon, vehicle_roi.lat, (double)vehicle_roi.alt);
					*control_data = &_control_data;
					_is_roi_set = true;
					_cur_roi_mode = vehicle_roi.mode;

				} else if (vehicle_roi.mode == vehicle_roi_s::ROI_TARGET) {
					//TODO is this even suported?
					exit_loop = false;

				} else {
					exit_loop = false;
				}
			}

			// check whether the position setpoint got updated
			if (polls[2].revents & POLLIN) {
				if (_cur_roi_mode == vehicle_roi_s::ROI_WPNEXT) {
					double lat, lon, alt = 0.;
					_read_lat_lon_alt_from_position_setpoint_sub(lon, lat, alt);
					_transform_lon_lat_to_angle(lon, lat, alt);
					*control_data = &_control_data;

				} else { // must do an orb_copy() in *every* case
					position_setpoint_triplet_s position_setpoint_triplet;
					orb_copy(ORB_ID(position_setpoint_triplet), _position_setpoint_triplet_sub, &position_setpoint_triplet);
					exit_loop = false;
				}
			}

			if (polls[3].revents & POLLIN) {
				vehicle_command_s vehicle_command;
				orb_copy(ORB_ID(vehicle_command), _vehicle_command_sub, &vehicle_command);

				// Process only if the command is for us or for anyone (component id 0).
				const bool sysid_correct = (vehicle_command.target_system == _mav_sys_id) || (vehicle_command.target_system == 0);
				const bool compid_correct = ((vehicle_command.target_component == _mav_comp_id) ||
							     (vehicle_command.target_component == 0));

				if (!sysid_correct || !compid_correct) {
					exit_loop = false;
					continue;
				}

				if (vehicle_command.command == vehicle_command_s::VEHICLE_CMD_DO_GIMBAL_MANAGER_ATTITUDE) {
					_set_control_data_from_set_attitude((uint32_t)vehicle_command.param5, vehicle_command.param3, vehicle_command.param1,
									    vehicle_command.param3, vehicle_command.param2);
					*control_data = &_control_data;
					_ack_vehicle_command(&vehicle_command);

				} else {
					exit_loop = false;
				}
			}
		}
	}

	return 0;
}

void InputMavlinkGimbalV2::_transform_lon_lat_to_angle(const double roi_lon, const double roi_lat,
		const double roi_alt)
{
	vehicle_global_position_s vehicle_global_position;
	_vehicle_global_position_sub.copy(&vehicle_global_position);
	const double &vlat = vehicle_global_position.lat;
	const double &vlon = vehicle_global_position.lon;

	_control_data.type = ControlData::Type::Angle;
	_control_data.type_data.angle.frames[0] = ControlData::TypeData::TypeAngle::Frame::AngleBodyFrame;
	_control_data.type_data.angle.frames[1] = ControlData::TypeData::TypeAngle::Frame::AngleBodyFrame;
	_control_data.type_data.angle.frames[2] = ControlData::TypeData::TypeAngle::Frame::AngleBodyFrame;

	_control_data.type_data.angle.angles[0] = 0.f;

	// interface: use fixed pitch value > -pi otherwise consider ROI altitude
	if (_control_data.type_data.lonlat.pitch_fixed_angle >= -M_PI_F) {
		_control_data.type_data.angle.angles[1] = _control_data.type_data.lonlat.pitch_fixed_angle;

	} else {
		_control_data.type_data.angle.angles[1] = _calculate_pitch(roi_lon, roi_lat, roi_alt, vehicle_global_position);
	}

	_control_data.type_data.angle.angles[2] = get_bearing_to_next_waypoint(vlat, vlon, roi_lat,
			roi_lon) - vehicle_global_position.yaw;

	// add offsets from VEHICLE_CMD_DO_SET_ROI_WPNEXT_OFFSET
	_control_data.type_data.angle.angles[1] += _control_data.type_data.lonlat.pitch_angle_offset;
	_control_data.type_data.angle.angles[2] += _control_data.type_data.lonlat.yaw_angle_offset;

	// make sure yaw is wrapped correctly for the output
	_control_data.type_data.angle.angles[2] = wrap_pi(_control_data.type_data.angle.angles[2]);
}

float InputMavlinkGimbalV2::_calculate_pitch(double lon, double lat, float altitude,
		const vehicle_global_position_s &global_position)
{
	if (!map_projection_initialized(&_projection_reference)) {
		map_projection_init(&_projection_reference, global_position.lat, global_position.lon);
	}

	float x1, y1, x2, y2;
	map_projection_project(&_projection_reference, lat, lon, &x1, &y1);
	map_projection_project(&_projection_reference, global_position.lat, global_position.lon, &x2, &y2);
	float dx = x1 - x2, dy = y1 - y2;
	float target_distance = sqrtf(dx * dx + dy * dy);
	float z = altitude - global_position.alt;

	return atan2f(z, target_distance);
}

void InputMavlinkGimbalV2::_read_lat_lon_alt_from_position_setpoint_sub(double &lon_sp, double &lat_sp, double &alt_sp)
{
	position_setpoint_triplet_s position_setpoint_triplet;
	orb_copy(ORB_ID(position_setpoint_triplet), _position_setpoint_triplet_sub, &position_setpoint_triplet);
	lon_sp = position_setpoint_triplet.current.lon;
	lat_sp = position_setpoint_triplet.current.lat;
	alt_sp = (double)position_setpoint_triplet.current.alt;
}

void InputMavlinkGimbalV2::_set_control_data_from_set_attitude(const uint32_t flags, const float pitch_angle,
		const float pitch_rate, const float yaw_angle, const float yaw_rate, float roll_angle, float roll_rate)
{

	if ((flags & gimbal_manager_set_attitude_s::GIMBAL_MANAGER_FLAGS_RETRACT) != 0) {
		// not implemented in ControlData
	} else if ((flags & gimbal_manager_set_attitude_s::GIMBAL_MANAGER_FLAGS_NEUTRAL) != 0) {
		_control_data.type = ControlData::Type::Neutral;

	} else if ((flags & gimbal_manager_set_attitude_s::GIMBAL_MANAGER_FLAGS_NONE) != 0) {
		// don't do anything
	} else {
		_control_data.type = ControlData::Type::Angle;
		_control_data.type_data.angle.frames[0] = ControlData::TypeData::TypeAngle::Frame::AngleBodyFrame;
		_control_data.type_data.angle.frames[1] = ControlData::TypeData::TypeAngle::Frame::AngleBodyFrame;
		_control_data.type_data.angle.frames[2] = ControlData::TypeData::TypeAngle::Frame::AngleBodyFrame;

		if (_is_roi_set && (flags & gimbal_manager_set_attitude_s::GIMBAL_MANAGER_FLAGS_NUDGE) != 0) {
			// add set_attitude.q to existing tracking angle or ROI
			// track message not yet implemented
			_control_data.type_data.angle.angles[0] += pitch_angle;
			_control_data.type_data.angle.angles[1] += roll_angle;
			_control_data.type_data.angle.angles[2] += yaw_angle;

		} else {
			_control_data.type_data.angle.angles[0] = pitch_angle;
			_control_data.type_data.angle.angles[1] = roll_angle;
			_control_data.type_data.angle.angles[2] = yaw_angle;
		}

		if (_is_roi_set && (flags & gimbal_manager_set_attitude_s::GIMBAL_MANAGER_FLAGS_OVERRIDE) != 0) {
			// overides tracking or ROI angle with set_attitude.q, respects flag GIMBAL_MANAGER_FLAGS_YAW_LOCK
			_control_data.type_data.angle.angles[0] = pitch_angle;
			_control_data.type_data.angle.angles[1] = roll_angle;
			_control_data.type_data.angle.angles[2] = yaw_angle;
		}

		if (PX4_ISFINITE(roll_rate)) { //roll
			_control_data.type_data.angle.frames[0] = ControlData::TypeData::TypeAngle::Frame::AngularRate;
			_control_data.type_data.angle.angles[0] = roll_rate; //rad/s
		}

		if (PX4_ISFINITE(pitch_rate)) { //pitch
			_control_data.type_data.angle.frames[1] = ControlData::TypeData::TypeAngle::Frame::AngularRate;
			_control_data.type_data.angle.angles[1] = pitch_rate; //rad/s
		}

		if (PX4_ISFINITE(yaw_rate)) { //yaw
			_control_data.type_data.angle.frames[2] = ControlData::TypeData::TypeAngle::Frame::AngularRate;
			_control_data.type_data.angle.angles[2] = yaw_rate; //rad/s
		}

		if (flags & gimbal_manager_set_attitude_s::GIMBAL_MANAGER_FLAGS_ROLL_LOCK) {
			// stay horizontal with the horizon
			_control_data.type_data.angle.frames[0] = ControlData::TypeData::TypeAngle::Frame::AngleAbsoluteFrame;
		}

		if (flags & gimbal_manager_set_attitude_s::GIMBAL_MANAGER_FLAGS_PITCH_LOCK) {
			_control_data.type_data.angle.frames[1] = ControlData::TypeData::TypeAngle::Frame::AngleAbsoluteFrame;
		}

		if (flags & gimbal_manager_set_attitude_s::GIMBAL_MANAGER_FLAGS_YAW_LOCK) {
			_control_data.type_data.angle.frames[2] = ControlData::TypeData::TypeAngle::Frame::AngleAbsoluteFrame;
		}
	}
}

//TODO move this one to input.cpp such that it can be shared across functions
void InputMavlinkGimbalV2::_ack_vehicle_command(vehicle_command_s *cmd)
{
	vehicle_command_ack_s vehicle_command_ack{};

	vehicle_command_ack.timestamp = hrt_absolute_time();
	vehicle_command_ack.command = cmd->command;
	vehicle_command_ack.result = vehicle_command_s::VEHICLE_CMD_RESULT_ACCEPTED;
	vehicle_command_ack.target_system = cmd->source_system;
	vehicle_command_ack.target_component = cmd->source_component;

	uORB::PublicationQueued<vehicle_command_ack_s> cmd_ack_pub{ORB_ID(vehicle_command_ack)};
	cmd_ack_pub.publish(vehicle_command_ack);
}

} /* namespace vmount */
