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

#include "open_manipulator_controller/open_manipulator_controller.h"

using namespace open_manipulator_controller;

OM_CONTROLLER::OM_CONTROLLER(std::string usb_port, std::string baud_rate)
    :node_handle_(""),
     priv_node_handle_("~"),
     tool_ctrl_flag_(false),
     comm_timer_thread_flag_(false),
     cal_thread_flag_(false),
     moveit_plan_flag_(false),
     using_platform_(false),
     using_moveit_(false),
     control_period_(0.010f),
     moveit_sampling_time_(0.050f),
     mutex_(PTHREAD_MUTEX_INITIALIZER)
{
  control_period_ = priv_node_handle_.param<double>("control_period", 0.010f);
  moveit_sampling_time_ = priv_node_handle_.param<double>("moveit_sample_duration", 0.050f);
  using_platform_ = priv_node_handle_.param<bool>("using_platform", false);
  using_moveit_ = priv_node_handle_.param<bool>("using_moveit", false);
  std::string planning_group_name = priv_node_handle_.param<std::string>("planning_group_name", "arm");

  open_manipulator_.initManipulator(using_platform_, usb_port, baud_rate);

  if (using_platform_ == true)        ROS_INFO("Succeeded to init %s", priv_node_handle_.getNamespace().c_str());
  else if (using_platform_ == false)  ROS_INFO("Ready to simulate %s on Gazebo", priv_node_handle_.getNamespace().c_str());

  if (using_moveit_ == true)
  {
    move_group_ = new moveit::planning_interface::MoveGroupInterface(planning_group_name);
    ROS_INFO("Ready to control %s group", planning_group_name.c_str());
  }
}

OM_CONTROLLER::~OM_CONTROLLER()
{
  comm_timer_thread_flag_ = false;
  pthread_join(comm_timer_thread_, NULL); // Wait for the thread associated with thread_p to complete
  RM_LOG::INFO("Shutdown the OpenManipulator");
  open_manipulator_.allActuatorDisable();
  ros::shutdown();
}

void OM_CONTROLLER::startCommTimerThread()
{
  ////////////////////////////////////////////////////////////////////
  /// Use this when you want to increase the priority of threads.
  ////////////////////////////////////////////////////////////////////
  //  pthread_attr_t attr_;
  //  int error;
  //  struct sched_param param;
  //  pthread_attr_init(&attr_);

  //  error = pthread_attr_setschedpolicy(&attr_, SCHED_RR);
  //  if (error != 0)   RM_LOG::ERROR("pthread_attr_setschedpolicy error = ", (double)error);
  //  error = pthread_attr_setinheritsched(&attr_, PTHREAD_EXPLICIT_SCHED);
  //  if (error != 0)   RM_LOG::ERROR("pthread_attr_setinheritsched error = ", (double)error);

  //  memset(&param, 0, sizeof(param));
  //  param.sched_priority = 31;    // RT
  //  error = pthread_attr_setschedparam(&attr_, &param);
  //  if (error != 0)   RM_LOG::ERROR("pthread_attr_setschedparam error = ", (double)error);

  //  int error;
  //  if ((error = pthread_create(&this->comm_timer_thread_, &attr_, this->commTimerThread, this)) != 0)
  //  {
  //    RM_LOG::ERROR("Creating timer thread failed!!", (double)error);
  //    exit(-1);
  //  }
  ////////////////////////////////////////////////////////////////////
  int error;
  if ((error = pthread_create(&this->comm_timer_thread_, NULL, this->commTimerThread, this)) != 0)
  {
    RM_LOG::ERROR("Creating timer thread failed!!", (double)error);
    exit(-1);
  }
  comm_timer_thread_flag_ = true;
}

void *OM_CONTROLLER::commTimerThread(void *param)
{
  OM_CONTROLLER *controller = (OM_CONTROLLER *) param;
  JointWayPoint tx_joint_way_point;
  std::vector<double> tx_tool_way_point;
  static struct timespec next_time;
  static struct timespec curr_time;

  clock_gettime(CLOCK_MONOTONIC, &next_time);

  while(controller->comm_timer_thread_flag_)
  {
    next_time.tv_sec += (next_time.tv_nsec + ((int)(controller->getControlPeriod() * 1000)) * 1000000) / 1000000000;
    next_time.tv_nsec = (next_time.tv_nsec + ((int)(controller->getControlPeriod() * 1000)) * 1000000) % 1000000000;

    pthread_mutex_lock(&(controller->mutex_));  // mutex lock

    if(controller->joint_way_point_buf_.size()) // get JointWayPoint for transfer to actuator
    {
      tx_joint_way_point = controller->joint_way_point_buf_.front();
      controller->present_joint_value = tx_joint_way_point;
      controller->joint_way_point_buf_.pop();
    }
    if(controller->tool_way_point_buf_.size())  // get ToolWayPoint for transfer to actuator
    {
      tx_tool_way_point = controller->tool_way_point_buf_.front();
      controller->tool_way_point_buf_.pop();
    }

    pthread_mutex_unlock(&(controller->mutex_)); // mutex unlock

    controller->open_manipulator_.communicationProcessToActuator(tx_joint_way_point, tx_tool_way_point);
    tx_joint_way_point.clear();
    tx_tool_way_point.clear();

    clock_gettime(CLOCK_MONOTONIC, &curr_time);

    /////
    double delta_nsec = (next_time.tv_sec - curr_time.tv_sec) + (next_time.tv_nsec - curr_time.tv_nsec)*0.000000001;
    if(delta_nsec < 0.0)
    {
      RM_LOG::WARN("Communication cycle time exceeded. : ", controller->getControlPeriod() - delta_nsec);
      next_time = curr_time;
    }
    else
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, NULL);
    /////
  }
  return 0;
}

void OM_CONTROLLER::startCalThread()
{
  int error;
  if ((error = pthread_create(&this->cal_thread_, NULL, this->calThread, this)) != 0)
  {
    RM_LOG::ERROR("Creating calculation thread failed!!", (double)error);
    exit(-1);
  }
  cal_thread_flag_ = true;
}

void *OM_CONTROLLER::calThread(void *param)
{
  OM_CONTROLLER *controller = (OM_CONTROLLER *) param;
  static struct timespec next_time;
  static struct timespec curr_time;
  static struct timespec temp_time;

  clock_gettime(CLOCK_MONOTONIC, &curr_time);
  double curr_time_s = curr_time.tv_sec + (curr_time.tv_nsec*0.000000001);

  next_time = curr_time;

  while(controller->cal_thread_flag_)
  {
    next_time.tv_sec += (next_time.tv_nsec + ((int)(controller->getControlPeriod() * 1000)) * 1000000) / 1000000000;
    next_time.tv_nsec = (next_time.tv_nsec + ((int)(controller->getControlPeriod() * 1000)) * 1000000) % 1000000000;
    double next_time_s = next_time.tv_sec + (next_time.tv_nsec*0.000000001);

    JointWayPoint tempJointWayPoint;
    std::vector<double> tempToolWayPoint;
    controller->open_manipulator_.calculationProcess(next_time_s, &tempJointWayPoint, &tempToolWayPoint);

    pthread_mutex_lock(&(controller->mutex_)); // mutex lock
    controller->joint_way_point_buf_.push(tempJointWayPoint);
    controller->tool_way_point_buf_.push(tempToolWayPoint);
    pthread_mutex_unlock(&(controller->mutex_)); // mutex unlock

    // debug
    clock_gettime(CLOCK_MONOTONIC, &temp_time);
    double delta_nsec = (next_time.tv_sec - temp_time.tv_sec) + (next_time.tv_nsec - temp_time.tv_nsec)*0.000000001;
    RM_LOG::INFO("control time : ", delta_nsec);

    if(controller->open_manipulator_.getTrajectoryMoveTime() < (next_time_s - curr_time_s))
      controller->cal_thread_flag_ = false;
  }
  return 0;
}

void OM_CONTROLLER::initPublisher()
{
  auto opm_tools_name = open_manipulator_.getManipulator()->getAllToolComponentName();

  for (auto const& name:opm_tools_name)
  {
    ros::Publisher pb;
    pb = priv_node_handle_.advertise<open_manipulator_msgs::KinematicsPose>(name + "/kinematics_pose", 10);
    open_manipulator_kinematics_pose_pub_.push_back(pb);
  }
  open_manipulator_state_pub_ = priv_node_handle_.advertise<open_manipulator_msgs::OpenManipulatorState>("states", 10);

  if(using_platform_ == true)
  {
    open_manipulator_joint_states_pub_ = priv_node_handle_.advertise<sensor_msgs::JointState>("joint_states", 10);
  }
  else
  {
    auto gazebo_joints_name = open_manipulator_.getManipulator()->getAllActiveJointComponentName();

    gazebo_joints_name.reserve(gazebo_joints_name.size() + opm_tools_name.size());

    gazebo_joints_name.insert(gazebo_joints_name.end(),
                            opm_tools_name.begin(),
                            opm_tools_name. end());

    for (auto const& name:gazebo_joints_name)
    {
      ros::Publisher pb;
      pb = priv_node_handle_.advertise<std_msgs::Float64>(name + "_position/command", 10);
      gazebo_goal_joint_position_pub_.push_back(pb);
    }
  }
}
void OM_CONTROLLER::initSubscriber()
{
  // msg subscriber
  open_manipulator_option_sub_ = priv_node_handle_.subscribe("option", 10, &OM_CONTROLLER::printManipulatorSettingCallback, this);
  if (using_moveit_ == true)
  {
    display_planned_path_sub_ = node_handle_.subscribe("/move_group/display_planned_path", 100,
                                              &OM_CONTROLLER::displayPlannedPathMsgCallback, this);
  }
}

void OM_CONTROLLER::initServer()
{
  goal_joint_space_path_server_                 = priv_node_handle_.advertiseService("goal_joint_space_path", &OM_CONTROLLER::goalJointSpacePathCallback, this);

  goal_task_space_path_server_                  = priv_node_handle_.advertiseService("goal_task_space_path", &OM_CONTROLLER::goalTaskSpacePathCallback, this);
  goal_task_space_path_position_only_server_    = priv_node_handle_.advertiseService("goal_task_space_path_position_only", &OM_CONTROLLER::goalTaskSpacePathPositionOnlyCallback, this);
  goal_task_space_path_orientation_only_server_ = priv_node_handle_.advertiseService("goal_task_space_path_orientation_only", &OM_CONTROLLER::goalTaskSpacePathOrientationOnlyCallback, this);

  goal_joint_space_path_to_present_server_      = priv_node_handle_.advertiseService("goal_joint_space_path_to_present", &OM_CONTROLLER::goalJointSpacePathToPresentCallback, this);

  goal_task_space_path_to_present_server_                   = priv_node_handle_.advertiseService("goal_task_space_path_to_present", &OM_CONTROLLER::goalTaskSpacePathToPresentCallback, this);
  goal_task_space_path_to_present_position_only_server_     = priv_node_handle_.advertiseService("goal_task_space_path_to_present_position_only", &OM_CONTROLLER::goalTaskSpacePathToPresentPositionOnlyCallback, this);
  goal_task_space_path_to_present_orientation_only_server_  = priv_node_handle_.advertiseService("goal_task_space_path_to_present_orientation_only", &OM_CONTROLLER::goalTaskSpacePathToPresentOrientationOnlyCallback, this);

  goal_tool_control_server_                 = priv_node_handle_.advertiseService("goal_tool_control", &OM_CONTROLLER::goalToolControlCallback, this);
  set_actuator_state_server_                = priv_node_handle_.advertiseService("set_actuator_state", &OM_CONTROLLER::setActuatorStateCallback, this);
  goal_drawing_trajectory_server_           = priv_node_handle_.advertiseService("goal_drawing_trajectory", &OM_CONTROLLER::goalDrawingTrajectoryCallback, this);

  if (using_moveit_ == true)
  {
    get_joint_position_server_  = priv_node_handle_.advertiseService("moveit/get_joint_position", &OM_CONTROLLER::getJointPositionMsgCallback, this);
    get_kinematics_pose_server_ = priv_node_handle_.advertiseService("moveit/get_kinematics_pose", &OM_CONTROLLER::getKinematicsPoseMsgCallback, this);
    set_joint_position_server_  = priv_node_handle_.advertiseService("moveit/set_joint_position", &OM_CONTROLLER::setJointPositionMsgCallback, this);
    set_kinematics_pose_server_ = priv_node_handle_.advertiseService("moveit/set_kinematics_pose", &OM_CONTROLLER::setKinematicsPoseMsgCallback, this);
  }
}

void OM_CONTROLLER::printManipulatorSettingCallback(const std_msgs::String::ConstPtr &msg)
{
  if(msg->data == "print_open_manipulator_setting")
    open_manipulator_.checkManipulatorSetting();
}

void OM_CONTROLLER::displayPlannedPathMsgCallback(const moveit_msgs::DisplayTrajectory::ConstPtr &msg)
{
  ROS_INFO("Get Moveit Planned Path");

  joint_trajectory_ = msg->trajectory[0].joint_trajectory;
  moveit_plan_flag_ = true;
}

bool OM_CONTROLLER::goalJointSpacePathCallback(open_manipulator_msgs::SetJointPosition::Request  &req,
                                               open_manipulator_msgs::SetJointPosition::Response &res)
{
  std::vector <double> target_angle;

  for(int i = 0; i < req.joint_position.joint_name.size(); i ++)
    target_angle.push_back(req.joint_position.position.at(i));

  pthread_mutex_lock(&mutex_); // mutex lock
  open_manipulator_.jointTrajectoryMove(target_angle, req.path_time, present_joint_value);
  pthread_mutex_unlock(&mutex_); // mutex unlock

  res.is_planned = true;
  return true;
}
bool OM_CONTROLLER::goalTaskSpacePathCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                              open_manipulator_msgs::SetKinematicsPose::Response &res)
{
  Pose target_pose;
  target_pose.position[0] = req.kinematics_pose.pose.position.x;
  target_pose.position[1] = req.kinematics_pose.pose.position.y;
  target_pose.position[2] = req.kinematics_pose.pose.position.z;

  Eigen::Quaterniond q(req.kinematics_pose.pose.orientation.w,
                        req.kinematics_pose.pose.orientation.x,
                        req.kinematics_pose.pose.orientation.y,
                        req.kinematics_pose.pose.orientation.z);

  target_pose.orientation = RM_MATH::convertQuaternionToRotation(q);
  open_manipulator_.taskTrajectoryMove(req.end_effector_name, target_pose, req.path_time);

  res.is_planned = true;
  return true;
}


bool OM_CONTROLLER::goalTaskSpacePathPositionOnlyCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                           open_manipulator_msgs::SetKinematicsPose::Response &res)
{
  Eigen::Vector3d position;
  position[0] = req.kinematics_pose.pose.position.x;
  position[1] = req.kinematics_pose.pose.position.y;
  position[2] = req.kinematics_pose.pose.position.z;

  open_manipulator_.taskTrajectoryMove(req.end_effector_name, position, req.path_time);

  res.is_planned = true;
  return true;
}

bool OM_CONTROLLER::goalTaskSpacePathOrientationOnlyCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                              open_manipulator_msgs::SetKinematicsPose::Response &res)
{
  Eigen::Matrix3d orientation;

  Eigen::Quaterniond q(req.kinematics_pose.pose.orientation.w,
                        req.kinematics_pose.pose.orientation.x,
                        req.kinematics_pose.pose.orientation.y,
                        req.kinematics_pose.pose.orientation.z);

  orientation = RM_MATH::convertQuaternionToRotation(q);
  open_manipulator_.taskTrajectoryMove(req.end_effector_name, orientation, req.path_time);

  res.is_planned = true;
  return true;
}

bool OM_CONTROLLER::goalJointSpacePathToPresentCallback(open_manipulator_msgs::SetJointPosition::Request  &req,
                                                        open_manipulator_msgs::SetJointPosition::Response &res)
{
  std::vector <double> target_angle;

  for(int i = 0; i < req.joint_position.joint_name.size(); i ++)
    target_angle.push_back(req.joint_position.position.at(i));

  open_manipulator_.jointTrajectoryMoveToPresentValue(target_angle, req.path_time);

  res.is_planned = true;
  return true;
}

bool OM_CONTROLLER::goalTaskSpacePathToPresentCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                                      open_manipulator_msgs::SetKinematicsPose::Response &res)
{
  Pose target_pose;
  target_pose.position[0] = req.kinematics_pose.pose.position.x;
  target_pose.position[1] = req.kinematics_pose.pose.position.y;
  target_pose.position[2] = req.kinematics_pose.pose.position.z;

  Eigen::Quaterniond q(req.kinematics_pose.pose.orientation.w,
                        req.kinematics_pose.pose.orientation.x,
                        req.kinematics_pose.pose.orientation.y,
                        req.kinematics_pose.pose.orientation.z);

  target_pose.orientation = RM_MATH::convertQuaternionToRotation(q);

  open_manipulator_.taskTrajectoryMoveToPresentPose(req.planning_group, target_pose, req.path_time);

  res.is_planned = true;
  return true;
}

bool OM_CONTROLLER::goalTaskSpacePathToPresentPositionOnlyCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                                    open_manipulator_msgs::SetKinematicsPose::Response &res)
{
  Eigen::Vector3d position;
  position[0] = req.kinematics_pose.pose.position.x;
  position[1] = req.kinematics_pose.pose.position.y;
  position[2] = req.kinematics_pose.pose.position.z;

  open_manipulator_.taskTrajectoryMoveToPresentPose(req.planning_group, position, req.path_time);

  res.is_planned = true;
  return true;
}

bool OM_CONTROLLER::goalTaskSpacePathToPresentOrientationOnlyCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                                       open_manipulator_msgs::SetKinematicsPose::Response &res)
{
  Eigen::Matrix3d orientation;
  Eigen::Quaterniond q(req.kinematics_pose.pose.orientation.w,
                        req.kinematics_pose.pose.orientation.x,
                        req.kinematics_pose.pose.orientation.y,
                        req.kinematics_pose.pose.orientation.z);

  orientation = RM_MATH::convertQuaternionToRotation(q);

  open_manipulator_.taskTrajectoryMoveToPresentPose(req.planning_group, orientation, req.path_time);

  res.is_planned = true;
  return true;
}

bool OM_CONTROLLER::goalToolControlCallback(open_manipulator_msgs::SetJointPosition::Request  &req,
                                            open_manipulator_msgs::SetJointPosition::Response &res)
{
  for(int i = 0; i < req.joint_position.joint_name.size(); i ++)
  {
    open_manipulator_.toolMove(req.joint_position.joint_name.at(i), req.joint_position.position.at(i));
  }
  res.is_planned = true;
  return true;
}

bool OM_CONTROLLER::setActuatorStateCallback(open_manipulator_msgs::SetActuatorState::Request  &req,
                                             open_manipulator_msgs::SetActuatorState::Response &res)
{
  if(req.set_actuator_state == true) // torque on
  {
    RM_LOG::INFO("Wait a second for actuator enable");
    comm_timer_thread_flag_ = false;
    pthread_join(comm_timer_thread_, NULL); // Wait for the thread associated with thread_p to complete
    open_manipulator_.allActuatorEnable();
    startCommTimerThread();
  }
  else // torque off
  {
    RM_LOG::INFO("Wait a second for actuator disable");
    comm_timer_thread_flag_ = false;
    pthread_join(comm_timer_thread_, NULL); // Wait for the thread associated with thread_p to complete
    open_manipulator_.allActuatorDisable();
    startCommTimerThread();
  }

  res.is_planned = true;
  return true;
}

bool OM_CONTROLLER::goalDrawingTrajectoryCallback(open_manipulator_msgs::SetDrawingTrajectory::Request  &req,
                                                  open_manipulator_msgs::SetDrawingTrajectory::Response &res)
{
  try
  {
    if(req.drawing_trajectory_name == "circle")
    {
      double draw_circle_arg[3];
      draw_circle_arg[0] = req.param[0];  // radius (m)
      draw_circle_arg[1] = req.param[1];  // revolution (rev)
      draw_circle_arg[2] = req.param[2];  // start angle position (rad)
      void* p_draw_circle_arg = &draw_circle_arg;
      open_manipulator_.drawingTrajectoryMove(DRAWING_CIRCLE, req.end_effector_name, p_draw_circle_arg, req.path_time);

    }
    else if(req.drawing_trajectory_name == "line")
    {
      Pose present_pose = open_manipulator_.getPose(req.end_effector_name);
      WayPoint draw_goal_pose[6];
      draw_goal_pose[0].value = present_pose.position(0) + req.param[0];
      draw_goal_pose[1].value = present_pose.position(1) + req.param[1];
      draw_goal_pose[2].value = present_pose.position(2) + req.param[2];
      draw_goal_pose[3].value = RM_MATH::convertRotationToRPY(present_pose.orientation)[0];
      draw_goal_pose[4].value = RM_MATH::convertRotationToRPY(present_pose.orientation)[1];
      draw_goal_pose[5].value = RM_MATH::convertRotationToRPY(present_pose.orientation)[2];

      void *p_draw_goal_pose = &draw_goal_pose;
      open_manipulator_.drawingTrajectoryMove(DRAWING_LINE, req.end_effector_name, p_draw_goal_pose, req.path_time);
    }
    else if(req.drawing_trajectory_name == "rhombus")
    {
      double draw_circle_arg[3];
      draw_circle_arg[0] = req.param[0];  // radius (m)
      draw_circle_arg[1] = req.param[1];  // revolution (rev)
      draw_circle_arg[2] = req.param[2];  // start angle position (rad)
      void* p_draw_circle_arg = &draw_circle_arg;
      open_manipulator_.drawingTrajectoryMove(DRAWING_RHOMBUS, req.end_effector_name, p_draw_circle_arg, req.path_time);
    }
    else if(req.drawing_trajectory_name == "heart")
    {
      double draw_circle_arg[3];
      draw_circle_arg[0] = req.param[0];  // radius (m)
      draw_circle_arg[1] = req.param[1];  // revolution (rev)
      draw_circle_arg[2] = req.param[2];  // start angle position (rad)
      void* p_draw_circle_arg = &draw_circle_arg;
      open_manipulator_.drawingTrajectoryMove(DRAWING_HEART, req.end_effector_name, p_draw_circle_arg, req.path_time);
    }
    res.is_planned = true;
    return true;
  }
  catch ( ros::Exception &e )
  {
    RM_LOG::ERROR("Creation the drawing trajectory is failed!");
  }

  return true;
}

bool OM_CONTROLLER::getJointPositionMsgCallback(open_manipulator_msgs::GetJointPosition::Request &req,
                                                open_manipulator_msgs::GetJointPosition::Response &res)
{
  ros::AsyncSpinner spinner(1);
  spinner.start();

  const std::vector<std::string> &joint_names = move_group_->getJointNames();
  std::vector<double> joint_values = move_group_->getCurrentJointValues();

  for (std::size_t i = 0; i < joint_names.size(); i++)
  {
    res.joint_position.joint_name.push_back(joint_names[i]);
    res.joint_position.position.push_back(joint_values[i]);
  }

  spinner.stop();
  return true;
}

bool OM_CONTROLLER::getKinematicsPoseMsgCallback(open_manipulator_msgs::GetKinematicsPose::Request &req,
                                                 open_manipulator_msgs::GetKinematicsPose::Response &res)
{
  ros::AsyncSpinner spinner(1);
  spinner.start();

  geometry_msgs::PoseStamped current_pose = move_group_->getCurrentPose();

  res.header                     = current_pose.header;
  res.kinematics_pose.pose       = current_pose.pose;

  spinner.stop();
  return true;
}

bool OM_CONTROLLER::setJointPositionMsgCallback(open_manipulator_msgs::SetJointPosition::Request &req,
                                                open_manipulator_msgs::SetJointPosition::Response &res)
{
  open_manipulator_msgs::JointPosition msg = req.joint_position;
  res.is_planned = calcPlannedPath(req.planning_group, msg);

  return true;
}

bool OM_CONTROLLER::setKinematicsPoseMsgCallback(open_manipulator_msgs::SetKinematicsPose::Request &req,
                                                 open_manipulator_msgs::SetKinematicsPose::Response &res)
{
  open_manipulator_msgs::KinematicsPose msg = req.kinematics_pose;
  res.is_planned = calcPlannedPath(req.planning_group, msg);

  return true;
}

bool OM_CONTROLLER::calcPlannedPath(const std::string planning_group, open_manipulator_msgs::KinematicsPose msg)
{
  ros::AsyncSpinner spinner(1);
  spinner.start();

  bool is_planned = false;
  geometry_msgs::Pose target_pose = msg.pose;

  move_group_->setPoseTarget(target_pose);

  move_group_->setMaxVelocityScalingFactor(msg.max_velocity_scaling_factor);
  move_group_->setMaxAccelerationScalingFactor(msg.max_accelerations_scaling_factor);

  move_group_->setGoalTolerance(msg.tolerance);

  moveit::planning_interface::MoveGroupInterface::Plan my_plan;

  if (open_manipulator_.isMoving() == false)
  {
    bool success = (move_group_->plan(my_plan) == moveit::planning_interface::MoveItErrorCode::SUCCESS);

    if (success)
    {
      is_planned = true;
    }
    else
    {
      ROS_WARN("Failed to Plan (task space goal)");
      is_planned = false;
    }
  }
  else
  {
    ROS_WARN("Robot is Moving");
    is_planned = false;
  }

  spinner.stop();

  return is_planned;
}

bool OM_CONTROLLER::calcPlannedPath(const std::string planning_group, open_manipulator_msgs::JointPosition msg)
{
  ros::AsyncSpinner spinner(1);
  spinner.start();

  bool is_planned = false;

  const robot_state::JointModelGroup *joint_model_group = move_group_->getCurrentState()->getJointModelGroup(planning_group);

  moveit::core::RobotStatePtr current_state = move_group_->getCurrentState();

  std::vector<double> joint_group_positions;
  current_state->copyJointGroupPositions(joint_model_group, joint_group_positions);

  for (uint8_t index = 0; index < msg.position.size(); index++)
  {
    joint_group_positions[index] = msg.position[index];
  }

  move_group_->setJointValueTarget(joint_group_positions);

  move_group_->setMaxVelocityScalingFactor(msg.max_velocity_scaling_factor);
  move_group_->setMaxAccelerationScalingFactor(msg.max_accelerations_scaling_factor);

  moveit::planning_interface::MoveGroupInterface::Plan my_plan;

  if (open_manipulator_.isMoving() == false)
  {
    bool success = (move_group_->plan(my_plan) == moveit::planning_interface::MoveItErrorCode::SUCCESS);

    if (success)
    {
      is_planned = true;
    }
    else
    {
      ROS_WARN("Failed to Plan (joint space goal)");
      is_planned = false;
    }
  }
  else
  {
    ROS_WARN("Robot is moving");
    is_planned = false;
  }

  spinner.stop();

  return is_planned;
}

void OM_CONTROLLER::publishOpenManipulatorStates()
{
  open_manipulator_msgs::OpenManipulatorState msg;
  if(open_manipulator_.isMoving())
    msg.open_manipulator_moving_state = msg.IS_MOVING;
  else
    msg.open_manipulator_moving_state = msg.STOPPED;

  if(open_manipulator_.isEnabled(JOINT_DYNAMIXEL))
    msg.open_manipulator_actuator_state = msg.ACTUATOR_ENABLED;
  else
    msg.open_manipulator_actuator_state = msg.ACTUATOR_DISABLED;

  open_manipulator_state_pub_.publish(msg);
}


void OM_CONTROLLER::publishKinematicsPose()
{
  open_manipulator_msgs::KinematicsPose msg;
  auto opm_tools_name = open_manipulator_.getManipulator()->getAllToolComponentName();

  uint8_t index = 0;
  for (auto const& tools:opm_tools_name)
  {
    Pose pose = open_manipulator_.getPose(tools);
    msg.pose.position.x = pose.position[0];
    msg.pose.position.y = pose.position[1];
    msg.pose.position.z = pose.position[2];
    Eigen::Quaterniond orientation = RM_MATH::convertRotationToQuaternion(pose.orientation);
    msg.pose.orientation.w = orientation.w();
    msg.pose.orientation.x = orientation.x();
    msg.pose.orientation.y = orientation.y();
    msg.pose.orientation.z = orientation.z();

    open_manipulator_kinematics_pose_pub_.at(index).publish(msg);
    index++;
  }
}

void OM_CONTROLLER::publishJointStates()
{
  sensor_msgs::JointState msg;
  msg.header.stamp = ros::Time::now();

  auto joints_name = open_manipulator_.getManipulator()->getAllActiveJointComponentName();
  auto tool_name = open_manipulator_.getManipulator()->getAllToolComponentName();

  auto joint_value = open_manipulator_.getAllActiveJointValue();
  auto tool_value = open_manipulator_.getAllToolValue();

  for(uint8_t i = 0; i < joints_name.size(); i ++)
  {
    msg.name.push_back(joints_name.at(i));

    msg.position.push_back(joint_value.at(i).value);
    msg.velocity.push_back(joint_value.at(i).velocity);
    msg.effort.push_back(joint_value.at(i).effort);
  }

  for(uint8_t i = 0; i < tool_name.size(); i ++)
  {
    msg.name.push_back(tool_name.at(i));

    msg.position.push_back(tool_value.at(i));
    msg.velocity.push_back(0.0f);
    msg.effort.push_back(0.0f);
  }
  open_manipulator_joint_states_pub_.publish(msg);
}

void OM_CONTROLLER::publishGazeboCommand()
{
  std::vector<WayPoint> joint_value = open_manipulator_.getAllActiveJointValue();
  std::vector<double> tool_value = open_manipulator_.getAllToolValue();

  for(uint8_t i = 0; i < joint_value.size(); i ++)
  {
    std_msgs::Float64 msg;
    msg.data = joint_value.at(i).value;

    gazebo_goal_joint_position_pub_.at(i).publish(msg);
  }

  for(uint8_t i = 0; i < tool_value.size(); i ++)
  {
    std_msgs::Float64 msg;
    msg.data = tool_value.at(i);

    gazebo_goal_joint_position_pub_.at(joint_value.size() + i).publish(msg);
  }
}

void OM_CONTROLLER::publishCallback(const ros::TimerEvent&)
{
  if (using_platform_ == true)  publishJointStates();
  else  publishGazeboCommand();

  publishOpenManipulatorStates();
  publishKinematicsPose();
}

void OM_CONTROLLER::moveitTimer(double present_time)
{
  static double priv_time = 0.0f;
  static uint32_t step_cnt = 0;

  if (moveit_plan_flag_ == true)
  {
    double path_time = present_time - priv_time;
    if (path_time > moveit_sampling_time_)
    {
      std::vector<WayPoint> target;
      uint32_t all_time_steps = joint_trajectory_.points.size();

      for(uint8_t i = 0; i < joint_trajectory_.points[step_cnt].positions.size(); i++)
      {
        WayPoint temp;
        temp.value = joint_trajectory_.points[step_cnt].positions.at(i);
        temp.velocity = joint_trajectory_.points[step_cnt].velocities.at(i);
        temp.acceleration = joint_trajectory_.points[step_cnt].accelerations.at(i);
        target.push_back(temp);
      }
      open_manipulator_.jointTrajectoryMove(target, path_time);

      step_cnt++;
      priv_time = present_time;

      if (step_cnt >= all_time_steps)
      {
        step_cnt = 0;
        moveit_plan_flag_ = false;
      }
    }
  }
  else
  {
    priv_time = present_time;
  }
}

void OM_CONTROLLER::process(double time)
{
  moveitTimer(time);
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "open_manipulator_controller");
  ros::NodeHandle node_handle("");

  std::string usb_port = "/dev/ttyUSB0";
  std::string baud_rate = "1000000";

  if (argc < 3)
  {
    ROS_ERROR("Please set '-port_name' and  '-baud_rate' arguments for connected Dynamixels");
    return 0;
  }
  else
  {
    usb_port = argv[1];
    baud_rate = argv[2];
  }

  OM_CONTROLLER om_controller(usb_port, baud_rate);

  om_controller.initPublisher();
  om_controller.initSubscriber();
  om_controller.initServer();

  om_controller.startCommTimerThread();

  ros::Timer publish_timer = node_handle.createTimer(ros::Duration(om_controller.getControlPeriod()), &OM_CONTROLLER::publishCallback, &om_controller);

  ros::Rate loop_rate(100);

  while (ros::ok())
  {
    ros::spinOnce();
    loop_rate.sleep();
  }

  return 0;
}
