#ifndef _ACTUATOR_PLUGIN_HH_
#define _ACTUATOR_PLUGIN_HH_
#include <functional>
#include <vector>
#include <string>
#include <thread>
#include "std_msgs/Float32.h"
#include <map>

// Gazebo
#include <gazebo/gazebo.hh>
#include <gazebo/common/common.hh>
#include <gazebo/physics/physics.hh>
#include <gazebo_plugins/gazebo_ros_utils.h>
#include <gazebo/transport/transport.hh>
#include <gazebo/msgs/msgs.hh>


// ROS
#include <ros/ros.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <geometry_msgs/Twist.h>
#include <geometry_msgs/Pose2D.h>
#include <nav_msgs/Odometry.h>
#include <sensor_msgs/JointState.h>
#include "ros/subscribe_options.h"
#include "ros/callback_queue.h"
#include <nav_msgs/Odometry.h>
#include <sensor_msgs/JointState.h>

// Boost
#include <boost/thread.hpp>
#include <boost/bind.hpp>


namespace gazebo
{
  /// \brief A plugin to control a Velodyne sensor.
  class ActuatorPlugin : public ModelPlugin
  {

    enum OdomSource
    {
        ENCODER = 0,
        WORLD = 1,
    };
    /// \brief Constructor
    public: ActuatorPlugin(){}
    /// \brief Destructor
    public: ~ActuatorPlugin()
    {

      alive_ = false;
      queue_.clear();
      queue_.disable();
      gazebo_ros_->node()->shutdown();
      callback_queue_thread_.join();
  
    }

    

    /// \brief The load function is called by Gazebo when the plugin is
    /// inserted into simulation
    /// \param[in] _model A pointer to the model that this plugin is
    /// attached to.
    /// \param[in] _sdf A pointer to the plugin's SDF element.
    public: void Load(physics::ModelPtr _model, sdf::ElementPtr _sdf)
    {
      
      std::string joint_name = "";
      if ( _sdf->HasElement("motor") ) 
      {
        for (sdf::ElementPtr elem = _sdf->GetElement("motor"); elem != NULL;
           elem = elem->GetNextElement("motor"))
        {
          if (!elem->HasElement("joint"))
          {
          gzwarn << "Invalid SDF: got motor without joint."
                 << std::endl;
          continue;
          }
          std::string joint_name = elem->Get<std::string>("joint");
          robot_namespace_ = "a";
          if (_sdf->HasElement("robotNamespace")) {
            robot_namespace_ = _sdf->GetElement("robotNamespace")->Get<std::string>();
          }
          if (_sdf->HasElement("commandTopicServo")) {
            command_topic_ = _sdf->GetElement("commandTopicServo")->Get<std::string>();
          }
          if (_sdf->HasElement("servoTorque")) {
            servo_torque = elem->Get<double>("servoTorque");
          }
          if (_sdf->HasElement("diameter_servo")) {
            servo_diameter = elem->Get<double>("diameter_servo");
          }
          gazebo_ros_->getParameter<double> ( update_rate_, "updateRate", 100.0 );
        }
      }
      else 
        {
            ROS_WARN_NAMED("ActuatorPlugin", "ActuatorPlugin missing <motor>!!!");
        }
      

      std::map<std::string, OdomSource> odomOptions;
      odomOptions["encoder"] = ENCODER;
      odomOptions["world"] = WORLD;
      gazebo_ros_->getParameter<OdomSource> ( odom_source_, "odometrySource", odomOptions, WORLD );

      // Store the model pointer for convenience.
      this->model = _model;
      gazebo_ros_ = GazeboRosPtr ( new GazeboRos ( _model, _sdf, "ActuPlug" ) );
      
      
      // Initialize ros, if it has not already bee initialized.
      if (!ros::isInitialized())
      {
        int argc = 0;
        char **argv = NULL;
        ros::init(argc, argv, "gazebo_client",
            ros::init_options::NoSigintHandler);
      }
      joints_=gazebo_ros_->getJoint(model, "joint", "joint");
      joints_->SetParam ( "fmax", 0, servo_torque );
      // Initialize update rate stuff
      if ( this->update_rate_ > 0.0 ) this->update_period_ = 1.0 / this->update_rate_;
      else this->update_period_ = 0.0;
      #if GAZEBO_MAJOR_VERSION >= 8
          last_update_time_ = model->GetWorld()->SimTime();
      #else
          last_update_time_ = model->GetWorld()->GetSimTime();
      #endif

      x_ = 0;
      rot_ = 0;
      alive_ = true;
      transform_broadcaster_ = boost::shared_ptr<tf::TransformBroadcaster>(new tf::TransformBroadcaster());

      // Initialize velocity stuff
      servo_speed_ = 0;
      
  
      // Initialize velocity support stuff
      servo_speed_instr_ = 0;
      
      // Get the first joint. We are making an assumption about the model
      // having one joint that is the rotational joint.
      // Store pointer to the joint we will actuate
      ROS_INFO("AAAAAAAAAAAAAAAAAAAAAAAAAAAA \n");



      // Create a topic name
      std::string subName = "/sg90_velcmd";
      ros::SubscribeOptions so =
        ros::SubscribeOptions::create<geometry_msgs::Twist>(command_topic_, 1,
                boost::bind(&ActuatorPlugin::cmdVelCallback, this, _1),
                ros::VoidPtr(), &queue_);

      cmd_vel_subscriber_ = gazebo_ros_->node()->subscribe(so);
      ROS_INFO("Actuator plugin ready");

      // start custom queue for actuator plugin
    this->callback_queue_thread_ =
        boost::thread ( boost::bind ( &ActuatorPlugin::QueueThread, this ) );
     // listen to the update event (broadcast every simulation iteration)
    this->update_connection_ =
        event::Events::ConnectWorldUpdateBegin ( boost::bind ( &ActuatorPlugin::UpdateChild, this ) );
   }
   

   private: void cmdVelCallback ( const geometry_msgs::Twist::ConstPtr& cmd_msg )
   {
     boost::mutex::scoped_lock scoped_lock ( lock );
     x_ = cmd_msg->linear.x;
     rot_ = cmd_msg->angular.z;
   }
   
   
    // Update the controller
    protected: void UpdateChild()
    {
    
        /* force reset SetParam("fmax") since Joint::Reset reset MaxForce to zero at
           https://bitbucket.org/osrf/gazebo/src/8091da8b3c529a362f39b042095e12c94656a5d1/gazebo/physics/Joint.cc?at=gazebo2_2.2.5#cl-331
           (this has been solved in https://bitbucket.org/osrf/gazebo/diff/gazebo/physics/Joint.cc?diff2=b64ff1b7b6ff&at=issue_964 )
           and Joint::Reset is called after ModelPlugin::Reset, so we need to set maxForce to wheel_torque other than GazeboRosDiffDrive::Reset
           (this seems to be solved in https://bitbucket.org/osrf/gazebo/commits/ec8801d8683160eccae22c74bf865d59fac81f1e)
        */
        
        if ( fabs(servo_torque -joints_->GetParam ( "fmax", 0 )) > 1e-6 ) {
          joints_->SetParam ( "fmax", 0, servo_torque );
        }
        
    
    
        if ( odom_source_ == ENCODER ) UpdateOdometryEncoder();
    #if GAZEBO_MAJOR_VERSION >= 8
        common::Time current_time = model->GetWorld()->SimTime();
    #else
        common::Time current_time = model->GetWorld()->GetSimTime();
    #endif
        double seconds_since_last_update = ( current_time - last_update_time_ ).Double();
    
        if ( seconds_since_last_update > update_period_ ) {
            // if (this->publish_tf_) publishOdometry ( seconds_since_last_update );
            // if ( publishWheelTF_ ) publishWheelTF();
            // if ( publishWheelJointState_ ) publishWheelJointState();
    
            // Update robot in case new velocities have been requested
            getServoVelocity();
    
            double current_speed;
    
            current_speed = joints_->GetVelocity ( 0 )   * ( servo_diameter / 2.0 );
    
            if ( servo_accel == 0 ||
                    ( fabs ( servo_speed_ - current_speed ) < 0.01 )) {
                //if max_accel == 0, or target speed is reached
                joints_->SetParam ( "vel", 0, servo_speed_/ ( servo_diameter / 2.0 ) );
            } else {
                if ( servo_speed_>=current_speed)
                    servo_speed_instr_+=fmin ( servo_speed_-current_speed,  servo_accel * seconds_since_last_update );
                else
                    servo_speed_instr_+=fmax ( servo_speed_-current_speed, -servo_accel * seconds_since_last_update );
                // ROS_INFO_NAMED("diff_drive", "actual wheel speed = %lf, issued wheel speed= %lf", current_speed[LEFT], wheel_speed_[LEFT]);
                // ROS_INFO_NAMED("diff_drive", "actual wheel speed = %lf, issued wheel speed= %lf", current_speed[RIGHT],wheel_speed_[RIGHT]);
    
                joints_->SetParam ( "vel", 0, servo_speed_instr_ / ( servo_diameter / 2.0 ) );
            }
        last_update_time_+= common::Time ( update_period_ );
    }
  }
    
    private: void getServoVelocity()
    {
        boost::mutex::scoped_lock scoped_lock ( lock );
    
        double vr = x_;
        double va = rot_;

        servo_speed_ = vr - va * servo_diameter / 2.0;
        
       
    }
    
    private: void UpdateOdometryEncoder()
    {
        double vel = joints_->GetVelocity ( 0 );
    #if GAZEBO_MAJOR_VERSION >= 8
        common::Time current_time = model->GetWorld()->SimTime();
    #else
        common::Time current_time = model->GetWorld()->GetSimTime();
    #endif
        double seconds_since_last_update = ( current_time - last_odom_update_ ).Double();
        last_odom_update_ = current_time;
    
        
    
        // Book: Sigwart 2011 Autonompus Mobile Robots page:337
        double s = vel * ( servo_diameter / 2.0 ) * seconds_since_last_update;
        // double sr = vr * ( wheel_diameter_ / 2.0 ) * seconds_since_last_update;
        double ssum = s;
    
        double sdiff;
        
    
        sdiff = s ;
        
    
        double dx = 0;
        double dy = 0;
        double dtheta = sdiff* ( servo_diameter / 2.0 );
    
        pose_encoder_.x += dx;
        pose_encoder_.y += dy;
        pose_encoder_.theta += dtheta;
    
        double w = dtheta/seconds_since_last_update;
        double v = sqrt ( dx*dx+dy*dy ) /seconds_since_last_update;
    
        tf::Quaternion qt;
        tf::Vector3 vt;
        qt.setRPY ( 0,0,pose_encoder_.theta );
        vt = tf::Vector3 ( pose_encoder_.x, pose_encoder_.y, 0 );
    
        odom_.pose.pose.position.x = vt.x();
        odom_.pose.pose.position.y = vt.y();
        odom_.pose.pose.position.z = vt.z();
    
        odom_.pose.pose.orientation.x = qt.x();
        odom_.pose.pose.orientation.y = qt.y();
        odom_.pose.pose.orientation.z = qt.z();
        odom_.pose.pose.orientation.w = qt.w();
    
        odom_.twist.twist.angular.z = w;
        odom_.twist.twist.linear.x = dx/seconds_since_last_update;
        odom_.twist.twist.linear.y = dy/seconds_since_last_update;
    }

    private: void Reset()
    {
    #if GAZEBO_MAJOR_VERSION >= 8
      last_update_time_ = model->GetWorld()->SimTime();
    #else
      last_update_time_ = model->GetWorld()->GetSimTime();
    #endif
      pose_encoder_.x = 0;
      pose_encoder_.y = 0;
      pose_encoder_.theta = 0;
      x_ = 0;
      rot_ = 0;
      joints_->SetParam ( "fmax", 0, servo_torque );
    }

    /// \brief ROS helper function that processes messages
    private: void QueueThread()
    {
    static const double timeout = 0.01;

      while ( alive_ && gazebo_ros_->node()->ok() ) {
          queue_.callAvailable ( ros::WallDuration ( timeout ) );
      }
    }
  public: 
    double current_joint_angle;  


  private: 
    double x_;
    double rot_;
    bool alive_;
    // Custom Callback Queue
    ros::CallbackQueue queue_;
    boost::thread callback_queue_thread_;
    // Update Rate
    double update_rate_;
    double update_period_;
    common::Time last_update_time_;
    
    OdomSource odom_source_;
    geometry_msgs::Pose2D pose_encoder_;
    common::Time last_odom_update_;
    event::ConnectionPtr update_connection_;
    
    double servo_speed_;
	  double servo_accel;
    double servo_diameter;
    double servo_speed_instr_;
    // std::vector<physics::JointPtr> joints_;
    double servo_torque;
    boost::mutex lock;

    // ROS STUFF
    ros::Publisher odometry_publisher_;
    ros::Subscriber cmd_vel_subscriber_;
    boost::shared_ptr<tf::TransformBroadcaster> transform_broadcaster_;
    sensor_msgs::JointState joint_state_;
    ros::Publisher joint_state_publisher_;
    nav_msgs::Odometry odom_;
    std::string tf_prefix_;
    std::string command_topic_;
    std::string robot_namespace_;
    GazeboRosPtr gazebo_ros_;
    ros::Publisher pub_;
    transport::SubscriberPtr sub;
    physics::JointPtr joints_;


    physics::ModelPtr model;
  

  };

// Tell Gazebo about this plugin, so that Gazebo can call Load on this plugin.
GZ_REGISTER_MODEL_PLUGIN(ActuatorPlugin)
}
#endif