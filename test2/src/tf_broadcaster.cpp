#include <ros/ros.h>
#include <tf/transform_broadcaster.h>

int main (int argc, char** argv){
	ros::init(argc, argv, "robot_tf_publisher");
	ros::NodeHandle n;
	
	ros::Rate r(100);
	// Here, we create a TransformBrocaster object that we'll use to send the base_link ->base_laser
	tf::TransformBroadcaster broadcaster;
	
	while(n.ok()){
		//there are five arguments required
		//First, we pass in the rotation transform, which is specified by a tbQuaternion for any rotation that needs to occur
		//between the to coordinate frames. (pitch, roll and yaw value value equal to zero for no rotation)
		//Second is a offset between base_link and base_laser
		//Third, we need to give the transform being published a timestamp
		//Fourth: we need to pass the name of parent node of the link we're creating
		//Filth, we need to pass the name of the child node of the link we're creating
		broadcaster.sendTransform(tf::StampedTransform(
											tf::Transform(tf::Quaternion(0, 0, 0, 1), tf::Vector3(0.138, 0, 0.42)),
											ros::Time::now(), "base_link", "laser_link"));
		r.sleep();
	}
}
