﻿/*******************************************************************************
* Copyright 2018 ROBOTIS CO., LTD.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

/* Authors: Darby Lim, Hye-Jong KIM, Ryan Shim, Yong-Ho Na */

#ifndef OPEN_MANIPULATOR_CONTROLLER_H
#define OPEN_MANIPULATOR_CONTROLLER_H

#include <ros/ros.h>
#include <boost/thread.hpp>
#include <unistd.h>
#include <queue>

// MoveIt! libs.
#include <moveit/move_group_interface/move_group_interface.h>
#include <moveit/robot_state/robot_state.h>
#include <moveit/planning_interface/planning_interface.h>
#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/robot_model/robot_model.h>
#include <moveit/robot_state/robot_state.h>

// msg & srv libs.
#include <sensor_msgs/JointState.h>
#include <geometry_msgs/PoseStamped.h>
#include <std_msgs/Float64.h>
#include <std_msgs/String.h>
#include <moveit_msgs/DisplayTrajectory.h>
#include <trajectory_msgs/JointTrajectory.h>
#include <trajectory_msgs/JointTrajectoryPoint.h>

#include "open_manipulator_msgs/SetJointPosition.h"
#include "open_manipulator_msgs/SetKinematicsPose.h"
#include "open_manipulator_msgs/SetDrawingTrajectory.h"
#include "open_manipulator_msgs/SetActuatorState.h"
#include "open_manipulator_msgs/GetJointPosition.h"
#include "open_manipulator_msgs/GetKinematicsPose.h"
#include "open_manipulator_msgs/OpenManipulatorState.h"

// open_manipulator libs.
#include "open_manipulator_libs/OpenManipulator.h"

namespace open_manipulator_controller
{

class OM_CONTROLLER
{
 private:
  // ROS NodeHandle
  ros::NodeHandle node_handle_;
  ros::NodeHandle priv_node_handle_;

  // ROS Parameters
  bool using_platform_;
  bool using_moveit_;
  double control_period_;
  double moveit_sampling_time_;

  // ROS Publisher
  ros::Publisher open_manipulator_state_pub_;
  std::vector<ros::Publisher> open_manipulator_kinematics_pose_pub_;
  ros::Publisher open_manipulator_joint_states_pub_;
  std::vector<ros::Publisher> gazebo_goal_joint_position_pub_;

  // ROS Subscribers
  ros::Subscriber open_manipulator_option_sub_;
  ros::Subscriber display_planned_path_sub_;

  // ROS Service Server
  ros::ServiceServer goal_joint_space_path_server_;
  ros::ServiceServer goal_task_space_path_server_;
  ros::ServiceServer goal_task_space_path_position_only_server_;
  ros::ServiceServer goal_task_space_path_orientation_only_server_;
  ros::ServiceServer goal_joint_space_path_to_present_server_;
  ros::ServiceServer goal_task_space_path_to_present_position_only_server_;
  ros::ServiceServer goal_task_space_path_to_present_orientation_only_server_;
  ros::ServiceServer goal_task_space_path_to_present_server_;
  ros::ServiceServer goal_tool_control_server_;
  ros::ServiceServer set_actuator_state_server_;
  ros::ServiceServer goal_drawing_trajectory_server_;
  ros::ServiceServer get_joint_position_server_;
  ros::ServiceServer get_kinematics_pose_server_;
  ros::ServiceServer set_joint_position_server_;
  ros::ServiceServer set_kinematics_pose_server_;

  // MoveIt! interface
  moveit::planning_interface::MoveGroupInterface* move_group_;
  trajectory_msgs::JointTrajectory joint_trajectory_;

  // Thread parameter
  pthread_t comm_timer_thread_;
  pthread_t cal_thread_;
  pthread_mutex_t mutex_;

  // shared variables
  std::queue<JointWayPoint> joint_way_point_buf_;
  std::queue<std::vector<double>> tool_way_point_buf_;
  std::vector<WayPoint> present_joint_value;

  // Related robotis_manipulator
  OPEN_MANIPULATOR open_manipulator_;

  // flag parameter
  bool tool_ctrl_flag_;
  bool comm_timer_thread_flag_;
  bool cal_thread_flag_;
  bool moveit_plan_flag_;

 public:
  OM_CONTROLLER(std::string usb_port, std::string baud_rate);
  ~OM_CONTROLLER();

  void publishCallback(const ros::TimerEvent&);

  void initPublisher();
  void initSubscriber();

  void initServer();

  void printManipulatorSettingCallback(const std_msgs::String::ConstPtr &msg);
  void displayPlannedPathMsgCallback(const moveit_msgs::DisplayTrajectory::ConstPtr &msg);

  double getControlPeriod(void){return control_period_;}

  bool goalJointSpacePathCallback(open_manipulator_msgs::SetJointPosition::Request  &req,
                                  open_manipulator_msgs::SetJointPosition::Response &res);

  bool goalTaskSpacePathCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                  open_manipulator_msgs::SetKinematicsPose::Response &res);

  bool goalTaskSpacePathPositionOnlyCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                             open_manipulator_msgs::SetKinematicsPose::Response &res);

  bool goalTaskSpacePathOrientationOnlyCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                                open_manipulator_msgs::SetKinematicsPose::Response &res);

  bool goalJointSpacePathToPresentCallback(open_manipulator_msgs::SetJointPosition::Request  &req,
                                           open_manipulator_msgs::SetJointPosition::Response &res);

  bool goalTaskSpacePathToPresentCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                          open_manipulator_msgs::SetKinematicsPose::Response &res);

  bool goalTaskSpacePathToPresentPositionOnlyCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                                      open_manipulator_msgs::SetKinematicsPose::Response &res);

  bool goalTaskSpacePathToPresentOrientationOnlyCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                                         open_manipulator_msgs::SetKinematicsPose::Response &res);

  bool goalToolControlCallback(open_manipulator_msgs::SetJointPosition::Request  &req,
                               open_manipulator_msgs::SetJointPosition::Response &res);

  bool setActuatorStateCallback(open_manipulator_msgs::SetActuatorState::Request  &req,
                              open_manipulator_msgs::SetActuatorState::Response &res);

  bool goalDrawingTrajectoryCallback(open_manipulator_msgs::SetDrawingTrajectory::Request  &req,
                                     open_manipulator_msgs::SetDrawingTrajectory::Response &res);

  bool setJointPositionMsgCallback(open_manipulator_msgs::SetJointPosition::Request &req,
                                   open_manipulator_msgs::SetJointPosition::Response &res);

  bool setKinematicsPoseMsgCallback(open_manipulator_msgs::SetKinematicsPose::Request &req,
                                    open_manipulator_msgs::SetKinematicsPose::Response &res);

  bool getJointPositionMsgCallback(open_manipulator_msgs::GetJointPosition::Request &req,
                                   open_manipulator_msgs::GetJointPosition::Response &res);

  bool getKinematicsPoseMsgCallback(open_manipulator_msgs::GetKinematicsPose::Request &req,
                                    open_manipulator_msgs::GetKinematicsPose::Response &res);

  void startCommTimerThread();
  static void *commTimerThread(void *param);

  void startCalThread();
  static void *calThread(void *param);

  void moveitTimer(double present_time);
  void process(double time);

  void publishOpenManipulatorStates();
  void publishKinematicsPose();
  void publishJointStates();
  void publishGazeboCommand();

  bool calcPlannedPath(const std::string planning_group, open_manipulator_msgs::JointPosition msg);
  bool calcPlannedPath(const std::string planning_group, open_manipulator_msgs::KinematicsPose msg);

};
}

#endif //OPEN_MANIPULATOR_CONTROLLER_H
