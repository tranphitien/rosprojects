#define ROS_MASTER_URI	"http:://localhost:11311"
#define ROS_ROOT	"opt/ros/hydro/share/ros"
//ROS library
#include <ros/ros.h>
#include <geometry_msgs/Twist.h>
#include <nav_msgs/Odometry.h>
#include <nav_msgs/Path.h>
#include <sensor_msgs/Imu.h>
#include <tf/transform_broadcaster.h>
#include <boost/assign/list_of.hpp>

//serial library
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>

#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#define DISTANCE_OF_2W	0.362
#define CONVERT_M_TO_ENC 100*9850/(10*M_PI)

#define DEFAULT_BAUDRATE 460800
#define BAUDMACRO	B460800
#define DEFAULT_SERIALPORT "/dev/ttyUSB0"
#define FPS	25 //40		//frames per second
#define RCV_LENGTH	17

static uint8_t cmd_frame[8];
static uint8_t rsp_frame[17];
static uint8_t cmd_field[3];
static uint16_t pkg_id;
bool find_mean;

static double left_path = 0, left_path1 = 0, right_path = 0, right_path1 = 0;
static double left_path_offset = 0, right_path_offset = 0;
static double x = 0, y = 0, theta = 0;
static double linear_x = 0, angular_z = 0;

double WHEEL_COVARIANCE = 1e-3;

void ucCommandCallback(const geometry_msgs::Twist::ConstPtr& msg){
	if(msg->linear.x != 0 || msg->angular.z != 0)
		cmd_frame[1] = 'R';
	else cmd_frame[1] = 'r';
	// Do some conversion to send speed values to MCU
	*(int16_t *)(cmd_frame + 4) = (int16_t)((msg->linear.x - msg->angular.z*0.181)*98500/M_PI);
	*(int16_t *)(cmd_frame + 6) = (int16_t)((msg->linear.x + msg->angular.z*0.181)*98500/M_PI);

	ROS_INFO("ROS Command: \n\
			 left_speed: %d\n\
			 right_speed: %d",
			 *(int16_t *)(cmd_frame + 4), *(int16_t *)(cmd_frame + 6));
}

int main (int argc, char** argv){
	ros::init(argc, argv, "tp_serial");
	ros::NodeHandle n;
	ROS_INFO("Serial server is starting");

	ros::Time current_time;

	tf::TransformBroadcaster transform_broadcaster;

	ros::Subscriber ucCommandMsg; // subscript to "cmd_vel" for receive command
	ros::Publisher odom_pub; // publish the odometry
	ros::Publisher path_pub;

	nav_msgs::Odometry move_base_odom;
	nav_msgs::Path pathMsg;

	static tf::Quaternion move_base_quat;

	//Initialize fixed values for odom and tf message
	move_base_odom.header.frame_id = "odom";
	move_base_odom.child_frame_id = "base_robot";

	//Reset all covariance values
	move_base_odom.pose.covariance = boost::assign::list_of(WHEEL_COVARIANCE)(0)(0)(0)(0)(0)
														   (0)(WHEEL_COVARIANCE)(0)(0)(0)(0)
														   (0)(0)(999999)(0)(0)(0)
														   (0)(0)(0)(999999)(0)(0)
														   (0)(0)(0)(0)(999999)(0)
														   (0)(0)(0)(0)(0)(WHEEL_COVARIANCE);
	move_base_odom.twist.covariance = boost::assign::list_of(WHEEL_COVARIANCE)(0)(0)(0)(0)(0)
															(0)(WHEEL_COVARIANCE)(0)(0)(0)(0)
															(0)(0)(999999)(0)(0)(0)
															(0)(0)(0)(999999)(0)(0)
															(0)(0)(0)(0)(999999)(0)
															(0)(0)(0)(0)(0)(WHEEL_COVARIANCE);
	pathMsg.header.frame_id = "odom";
	//----------ROS Odometry publishing------------------

	//----------Initialize serial connection
	ROS_INFO("Serial Initialing:\n Port: %s \n BaudRate: %d", DEFAULT_SERIALPORT, DEFAULT_BAUDRATE);
	int fd = -1;
	struct termios newtio;
	FILE *fpSerial = NULL;
	// read/write, not controlling terminal for process
	fd = open(DEFAULT_SERIALPORT, O_RDWR | O_NOCTTY | O_NDELAY);
	if (fd < 0){
		ROS_ERROR("Serial Init: Could not open serial device %s", DEFAULT_SERIALPORT);
		return 0;
	}
	//set up new setting
	memset(&newtio, 0, sizeof(newtio));
	newtio.c_cflag = CS8 | CLOCAL | CREAD; //8bit, no parity, 1 stop bit
	newtio.c_iflag |= IGNBRK; //ignore break codition
	newtio.c_oflag = 0;	//all options off
	//newtio.c_lflag = ICANON; //process input as lines
	newtio.c_cc[VTIME] = 0;
	newtio.c_cc[VMIN] = 20; // byte readed per a time
	//active new setting
	tcflush(fd, TCIFLUSH);

	if (cfsetispeed(&newtio, BAUDMACRO) < 0 || cfsetospeed(&newtio, BAUDMACRO)){
		ROS_ERROR("Serial Init: Failed to set serial BaudRate: %d", DEFAULT_BAUDRATE);
		close(fd);
		return 0;
	}
	ROS_INFO("Connection established with %s at %d.", DEFAULT_SERIALPORT, BAUDMACRO);
	tcsetattr(fd, TCSANOW, &newtio);
	tcflush(fd, TCIOFLUSH);

	// Open file as a standard I/O stream
	fpSerial = fdopen(fd, "r+");
	if (!fpSerial){
		ROS_ERROR("Serial Init: Failed to open serial stream %s", DEFAULT_SERIALPORT);
		fpSerial = NULL;
	}

	//Creating message to talk with ROS
	//Subscribe to ROS message
	ucCommandMsg = n.subscribe<geometry_msgs::Twist>("cmd_vel", 1, ucCommandCallback);
	//Setup to publish ROS message
	odom_pub = n.advertise<nav_msgs::Odometry>("tp_robot/odom", 10);
	path_pub = n.advertise<nav_msgs::Path>("tp_robot/odomPath", 10);

	// An "adaptive" timer to maintain periodical communication with the MCU
	ros::Rate rate(FPS);
	uint8_t i = FPS;
	while (i--){
		find_mean = true;
		rate.sleep();
		ros::spinOnce();
	}
	find_mean = false;

	//Loop for input
	while(ros::ok()){
		//Process the callbacks
		ros::spinOnce();
		//Impose command and get back respone
		int res;
		cmd_frame[0] = 'f';
		//cmd_frame[1] = 'T';
		*(uint16_t *)(cmd_frame + 2) = pkg_id;
		//*(uint16_t *)(cmd_frame + 4) = left_speed; //set left speed
		//*(uint16_t *)(cmd_frame + 6) = right_speed; // set right speed
		res = write(fd, cmd_frame, 8);
		if (res < 8) ROS_ERROR("Error sending command");
		current_time = ros::Time::now();
		//sleep to wait the response is returned
		rate.sleep();

		//Read a number of RCV_LENGHT chars as if they have arrived
		res = read(fd, rsp_frame, RCV_LENGTH);
		if (res < RCV_LENGTH){
			//ROS_INFO("frame %d dropped", pkg_id);
			while(read(fd, rsp_frame, 1) > 0);
		}
		else{
			uint16_t rcv_pkg_id = *(uint16_t *)(rsp_frame +2);
			if(rcv_pkg_id == pkg_id){
				//read the commands to display
				cmd_field[0] = rsp_frame[0];
				cmd_field[1] = rsp_frame[1];
				cmd_field[2] = 0;

				//Time sent from RTOS to check if the moving base has been reset
				uint8_t reset_robot = *(uint8_t *)(rsp_frame + 16);
				//Calculate the x,y,z,v,w odometry data
				double left_speed = *(int16_t *)(rsp_frame + 4)*M_PI/98500;
				double right_speed = *(int16_t *)(rsp_frame + 6)*M_PI/98500;

				//Get the newly traveled distance on each wheel
				if (rcv_pkg_id == 0 || reset_robot == 1 ){
					left_path_offset = *(int32_t *)(rsp_frame +8)*M_PI/98500; 
					right_path_offset = *(int32_t *)(rsp_frame + 12)*M_PI/98500;
					left_path = 0;
					right_path = 0;
				}
				else{
					left_path = *(int32_t *)(rsp_frame +8)*M_PI/98500 - left_path_offset;
					right_path = *(int32_t *)(rsp_frame + 12)*M_PI/98500 - right_path_offset;
				}

				//Calculate and publish the odometry data
				double left_delta = left_path - left_path1;
				double right_delta = right_path - right_path1;

				//only if the robot has traveled a significant distance will be calculate the odometry
				if(!((left_delta < 0.0075 && left_delta > -0.0075) && (right_delta < 0.0075 && right_delta > -0.0075))){
					//Calculate the pose in reference to world base
					theta += (right_delta - left_delta)/0.3559;

					//translate theta to [-pi; pi]
					if(theta > (2*M_PI)) theta -= 2*M_PI;
					if (theta < 0) theta += 2*M_PI;

					//Calculate displacement
					double disp = (right_delta + left_delta)/2;
					x += disp*cos(theta);
					y += disp*sin(theta);

					//Calculate the twist in reference to the robot base
					linear_x = (right_speed + left_speed)/2;
					angular_z = (right_speed - left_speed)/0.362;

					left_path1 = left_path;
					right_path1 = right_path;

				}
    
				// Infer the rotation angle and publish transform
				move_base_quat = tf::Quaternion(tf::Vector3(0, 0, 1), theta);
				transform_broadcaster.sendTransform(tf::StampedTransform(
													tf::Transform(move_base_quat, tf::Vector3(x,y,0)),
													current_time, "odom", "base_robot"));

				//Publish the odometry message over ROS
				move_base_odom.header.stamp = current_time;
				//move_base_odom.header.stamp = ros::Time::now();
				//set the position
				move_base_odom.pose.pose.position.x = x;
				move_base_odom.pose.pose.position.y = y;
				move_base_odom.pose.pose.position.z = 0;
				move_base_odom.pose.pose.orientation.x = move_base_quat.x();
				move_base_odom.pose.pose.orientation.y = move_base_quat.y();
				move_base_odom.pose.pose.orientation.z = move_base_quat.z();
				move_base_odom.pose.pose.orientation.w = move_base_quat.w();

				//Set the velocity
				move_base_odom.child_frame_id = "base_robot";
				move_base_odom.twist.twist.linear.x = linear_x;
				move_base_odom.twist.twist.linear.y = 0;
				move_base_odom.twist.twist.angular.z = angular_z;

				//Push back the pose to path message
				pathMsg.header = move_base_odom.header;
				geometry_msgs::PoseStamped pose_stamped;
				pose_stamped.header = move_base_odom.header;
				pose_stamped.pose = move_base_odom.pose.pose;
				pathMsg.poses.push_back(pose_stamped);
				//publish the odom and path message
				odom_pub.publish(move_base_odom);
				path_pub.publish(pathMsg);

				//ROS_INFO("MCU responded: %s\t%d\t%f\t%f\t%f\t%f\t%d", cmd_field,\
																rcv_pkg_id,\
																left_speed,\
																right_speed,\
																left_path,\
																right_path,\
																reset_robot);

			}
			else{
				ROS_INFO("frame %d corrupted !", pkg_id);
				while(read(fd, rsp_frame, 1) > 0);
			}
		}
		//Increase pkg_id for next loop's frame check
		pkg_id ++;
	}
	//Process ROS message and send serial commands to uController
	ROS_INFO("Serial server stopped.");
	return 0;
}
