
#include <ros/ros.h>

#include <math.h>
#include <neural_network_planner/build_database.h>

#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>


#include <string>
#include <ctime>
#include <cmath>

#include <boost/lexical_cast.hpp>
#include <iostream>

#include "boost/scoped_ptr.hpp"
#include "caffe/proto/caffe.pb.h"
#include "caffe/util/db.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/rng.hpp"
#include "caffe/util/format.hpp"

#include <cstdio>
#include <cstdlib>


using namespace sensor_msgs;
using namespace nav_msgs;
using namespace move_base_msgs;
using namespace message_filters;
using namespace geometry_msgs;
using namespace visualization_msgs;	


using boost::lexical_cast;
using boost::scoped_ptr;
using std::cout;
using std::endl;

const int labels_size = 1;

namespace neural_network_planner {


BuildDatabase::BuildDatabase(std::string& database_name) : private_nh("~")
{


	private_nh.param("set_size", set_size, 10000);
	private_nh.param("batch_size", batch_size, 100);	
	private_nh.param("scan_topic", scan_topic, std::string("/base_scan") );
	private_nh.param("goal_topic", goal_topic, std::string("/move_base/goal") );	
	private_nh.param("odom_topic", odom_topic, std::string("/odom") );
	private_nh.param("base_path", base_path, std::string("") );
	private_nh.param("averaged_ranges_size", averaged_ranges_size, 15 );
	private_nh.param("database_backend", backend, std::string("leveldb"));
	private_nh.param("logs_path", logs_path, std::string(""));
	private_nh.param<float>("sampling_rate", sampling_rate, 1);
	private_nh.param<float>("pos_update_threshold", pos_update_threshold, 0.001);
	private_nh.param("show_lines", show_lines, false);
	private_nh.param<float>("target_tolerance", target_tolerance, 0.08);
	private_nh.param("steer_feedback", steer_feedback, true);
	private_nh.param<float>("dist_preweight", dist_preweight, 1);
	private_nh.param<float>("angle_preweight", angle_preweight, 1);
	private_nh.param<float>("step_resolution", step_resolution, 0.10); 
	private_nh.param("yaw_resolution", yaw_resolution, 10); 
	
	FLAGS_log_dir = logs_path;
	FLAGS_alsologtostderr = 1;
	FLAGS_minloglevel = 0;

	initializeSteer(steer_angles, yaw_resolution);
	
	prev_target = std::pair<float, float>(0,0);
	current_source = std::pair<float,float>(0,0);
	prev_source = std::pair<float,float>(0,0);
	
	range_data = vector<float>(averaged_ranges_size, 0);

	yaw_measured = prev_yaw_measured = 0;

	// related neural network input size selected for this build_database run 

	if ( steer_feedback )
		state_sequence_size = averaged_ranges_size + 3;

	timestep = database_counter = 0;
	db_writestep = 0;

	CHECK_EQ(set_size % batch_size, 0) << "set_size must be multiple of batch_size!";

	ros::NodeHandle db_nh("build_db");
	laserscan_sub_.subscribe(db_nh, scan_topic, 25);
	odom_sub_.subscribe(db_nh, odom_topic, 25);

	typedef sync_policies::ApproximateTime<sensor_msgs::LaserScan, nav_msgs::Odometry> StepPolicy;
	
	Synchronizer<StepPolicy> step_sync( StepPolicy(10), laserscan_sub_, odom_sub_);
	step_sync.registerCallback(boost::bind(&neural_network_planner::BuildDatabase::build_callback, this, _1, _2));

	ros::NodeHandle nh;
	goal_sub_ = nh.subscribe<MoveBaseActionGoal>(goal_topic , 1, boost::bind(&BuildDatabase::updateTarget_callback, this, _1));

	net_ranges_pub_ = nh.advertise<LaserScan>("state_ranges", 1);
	if( show_lines ) {
		marker_pub_ = nh.advertise<Marker>("range_lines", 1);
	}

	time_t init = time(0);
	tm *init_tm = localtime(&init);	

	// dataset initialization
	std::string yaw_db_path = base_path + "yaw_db-" + lexical_cast<std::string>(init_tm->tm_mon+1) 
				              + "-" + lexical_cast<std::string>(init_tm->tm_mday) + "-" + lexical_cast<std::string>(init_tm->tm_hour) 
				              + "-" + lexical_cast<std::string>(init_tm->tm_min) + "_" + backend;
	scoped_ptr<caffe::db::DB> yaw_database(caffe::db::GetDB(backend));
	yaw_database->Open(yaw_db_path, caffe::db::NEW);
	scoped_ptr<caffe::db::Transaction> yaw_txn(yaw_database->NewTransaction());

	std::string steer_db_path = base_path + "steer_db-" + lexical_cast<std::string>(init_tm->tm_mon+1) 
				              + "-" + lexical_cast<std::string>(init_tm->tm_mday) + "-" + lexical_cast<std::string>(init_tm->tm_hour) 
				              + "-" + lexical_cast<std::string>(init_tm->tm_min) + "_" + backend;
	scoped_ptr<caffe::db::DB> steer_database(caffe::db::GetDB(backend));
	steer_database->Open(steer_db_path, caffe::db::NEW);
	scoped_ptr<caffe::db::Transaction> steer_txn(steer_database->NewTransaction());

	// creating a text file to debug database building - just first times
     // to check everything is right

	std::string check_text = base_path + "check_db-" + lexical_cast<std::string>(init_tm->tm_mon+1) 
				         + "-" + lexical_cast<std::string>(init_tm->tm_mday) + "-" + lexical_cast<std::string>(init_tm->tm_hour) 
				         + "-" + lexical_cast<std::string>(init_tm->tm_min) + "_" + backend;

	int print_check;
	FILE * table = fopen(check_text.c_str(), "w");
	if( table == NULL ) {
		cout << "Plot file opening failed.\n";
		exit(1);
	}
	fprintf(table, "PREWEIGHTS: DIST %.3f  ANGLE  %.3f \n", dist_preweight, angle_preweight);

	fclose(table);


	goal_received = false;

	LOG(INFO) << "DATASET INITIALIZED: sequence length: " << state_sequence_size;

	FLAGS_minloglevel = 1;

	ros::Rate store_rate(sampling_rate);

	while( ros::ok() && db_writestep < set_size) {

	  if(  ( fabs(meas_linear_x) > noise_level || fabs(meas_angular_z) > noise_level )
			 && goal_received && Step_dist() > step_resolution) { 

		float x_rel = current_target.first - current_source.first;
		float y_rel = current_target.second - current_source.second;	 

		float distance = hypot( x_rel, y_rel);
		float relative_angle = atan2( y_rel , x_rel );

		std::string yaw_value;
		caffe::Datum yaw_datum;
		yaw_datum.set_channels(state_sequence_size);
		yaw_datum.set_height(1);
		yaw_datum.set_width(1);
		
		// storing the state data 				
		for(int i = 0; i < range_data.size(); i++) {
			yaw_datum.add_float_data(range_data[i]);
		}		

		yaw_datum.add_float_data(distance * dist_preweight);
		yaw_datum.add_float_data(relative_angle * angle_preweight);

		if( steer_feedback ) {
			yaw_datum.add_float_data(prev_yaw_measured);
		}

		yaw_datum.set_label(yaw_measured);

		yaw_datum.set_encoded(false);
		yaw_datum.SerializeToString(&yaw_value);	
		std::string key_str = caffe::format_int(db_writestep, 8);
		yaw_txn->Put(key_str, yaw_value);

		std::string steer_value;
		caffe::Datum steer_datum;
		steer_datum.set_channels(state_sequence_size);
		steer_datum.set_height(1);
		steer_datum.set_width(1);
		
		// storing the state data 				
		for(int i = 0; i < range_data.size(); i++) {
			steer_datum.add_float_data(range_data[i]);
		}		

		steer_datum.add_float_data(distance * dist_preweight);
		steer_datum.add_float_data(relative_angle * angle_preweight);

		if( steer_feedback ) {

			steer_datum.add_float_data(prev_closest_steer);
				
		}

		int steer_label;
		float closest_steer = 0;
		
		steer_label = getClosestSteer(steer_angles, yaw_measured, closest_steer);
		
//		steer_datum.set_label(closest_steer);

		steer_datum.set_label(steer_label);

		steer_datum.set_encoded(false);
		steer_datum.SerializeToString(&steer_value);	
		steer_txn->Put(key_str, steer_value);
				
		LOG(INFO) << "Label data: index " << steer_label 
				<< " index_value: " << closest_steer
				<< " current yaw: " << yaw_measured;

		table = fopen(check_text.c_str(), "a");
		if( table == NULL ) {
			cout << "Plot file opening failed.\n";
			exit(1);
		}
		for(int i = 0; i < range_data.size(); i++) {
		 fprintf(table, "%.4f   ", range_data[i]); 
		}
		 fprintf(table, "%.4f   ", distance * dist_preweight); 
		 fprintf(table, "%.4f \n  ", relative_angle); 
		 fprintf(table, "DB STEP %d   LABEL   %.4f  %.4f  %d \n ", db_writestep, 
						yaw_measured, closest_steer, steer_label); 
		 fclose(table);
		
		LOG_EVERY_N(WARNING, 1000) << "Stored step  " << db_writestep;

		if( ++db_writestep % batch_size == 0 ) { // commit the batch to dbs

			yaw_txn->Commit();			
			yaw_txn.reset(yaw_database->NewTransaction());
			steer_txn->Commit();			
			steer_txn.reset(steer_database->NewTransaction());

      	} 
		
		// update the tails
		prev_yaw_measured = yaw_measured;
		prev_closest_steer = closest_steer;	
			
	  } // check available step
	  

	  prev_source = current_source;

	  if ( point_distance(current_source, current_target) <= target_tolerance ) {
		 // current pos has achieved target within tolerance
		LOG(INFO) << "target reached";
		goal_received = false;
	  }

	  store_rate.sleep();
	
	  ros::spinOnce();

	}

	
	if( db_writestep % batch_size != 0) { // Last commit if needed for a safe closing

		LOG(INFO) << "Closing DATABASES ";
		yaw_txn->Commit();	
		steer_txn->Commit();
	}


	
}

BuildDatabase::~BuildDatabase() {


}

	

/* this thread is collecting navigation data - first variant - labels are set depending
 * on the real path effectively taken by the robot - output of controller (local planner)
 */
void BuildDatabase::build_callback(const LaserScan::ConstPtr& laser_msg, 
							const Odometry::ConstPtr& odom_msg)
{
	
	timestep++;
//	ROS_INFO("Database_callback timestep: %d", timestep);
	
	// update measured robot position
	tmp_source.first = odom_msg->pose.pose.position.x;
	tmp_source.second = odom_msg->pose.pose.position.y;


	if ( point_distance(current_source, tmp_source) > pos_update_threshold ) 
		current_source = tmp_source;

	tf::Pose pose;
     tf::poseMsgToTF(odom_msg->pose.pose, pose);
     yaw_measured = tf::getYaw(pose.getRotation());

	meas_linear_x = odom_msg->twist.twist.linear.x;
	meas_angular_z = odom_msg->twist.twist.angular.z;
	
	vector<float> ranges = laser_msg->ranges;
	
	float range_num = ranges.size();
	vector<float> current_ranges;
	
	int average_base = range_num / averaged_ranges_size;

	float add_range;	
		
	for(int i = 0; i < range_num; i++) {

		add_range += ranges[i];

		if( (i+1) % average_base == 0 ) {
			current_ranges.push_back( add_range / average_base );
			add_range = 0;
		}
			
	}		


	CHECK_EQ(current_ranges.size(), averaged_ranges_size);

	for(int i = 0; i < current_ranges.size(); i++) {
		range_data[i] = current_ranges[i];
	}
	
		
	float start_angle = (float) (laser_msg->angle_max - laser_msg->angle_min) / (averaged_ranges_size*2);
 
	state_ranges.angle_min = laser_msg->angle_min + start_angle;
	state_ranges.angle_max = laser_msg->angle_max;
	state_ranges.range_min = laser_msg->range_min;
	state_ranges.range_max = laser_msg->range_max;
	state_ranges.header.stamp = line_list.header.stamp = ros::Time::now();
	state_ranges.ranges = range_data;
	state_ranges.angle_increment = (state_ranges.angle_max - state_ranges.angle_min) / averaged_ranges_size;
	state_ranges.header.frame_id = "hokuyo_link";
	line_list.header.frame_id = "/odom";
	line_list.action = visualization_msgs::Marker::ADD;
	line_list.pose.orientation.w = 1.0;
	line_list.id = 0;
	line_list.type = visualization_msgs::Marker::LINE_LIST;
	line_list.color.r = 1.0;
     line_list.color.a = 1.0;

	geometry_msgs::Point p_source;
	p_source.x = tmp_source.first;
	p_source.y = tmp_source.second;

	if( show_lines ) {

		for(int i = 0; i < current_ranges.size(); i++) {

			geometry_msgs::Point p;
			p.x = p_source.x + current_ranges[i] * cos(state_ranges.angle_min + state_ranges.angle_increment * i);
			p.y = p_source.y + current_ranges[i] * sin(state_ranges.angle_min + state_ranges.angle_increment * i);
			p.z = 0.025;

			line_list.points.push_back(p);
			line_list.points.push_back(p_source);

		}
		
		marker_pub_.publish(line_list);
	}

	net_ranges_pub_.publish(state_ranges);	

}

void BuildDatabase::updateTarget_callback( const MoveBaseActionGoal::ConstPtr& actiongoal_msg) {

	LOG(INFO) << "Updating target pose";
	current_target.first = actiongoal_msg->goal.target_pose.pose.position.x;
	current_target.second = actiongoal_msg->goal.target_pose.pose.position.y;
	goal_received = true;

}

void BuildDatabase::updateCmdVel_callback( const geometry_msgs::Twist::ConstPtr& cmdvel_msg ) {

	cmdvel_time = ros::Time::now();
//	LOG(INFO) << "Updating ref commands from navigation stack";
	ref_linear_x = cmdvel_msg->linear.x;
	ref_angular_z = cmdvel_msg->angular.z;

}

float BuildDatabase::Step_dist() {

	return hypot(current_source.second - prev_source.second, current_source.first - prev_source.first);

}

float point_distance(std::pair<float, float>& start_point, std::pair<float, float>& end_point) 
{

	return hypot(end_point.second - start_point.second, end_point.first - start_point.first);

}

int  saturate(float neg_lim, float pos_lim, float& value) 
 {

	if( value < neg_lim ) { 
		value = neg_lim;
		return 1;
	}
	else if( value > pos_lim ) {
		value = pos_lim;
	  	return 2;
	}
	else 
		return 0;

 }


void initializeSteer(vector<float>& steer_angles, int yaw_resolution)

{

	CHECK_EQ(360 % yaw_resolution, 0) << "Please set a yaw resolution that is a divider of 360 degrees";

	float yaw_res_rad = ( yaw_resolution * M_PI ) / 180;

	float add_allowed = 0;
	steer_angles.push_back(add_allowed);
	//LOG(INFO) << "Allowed steer angles: " << add_allowed;

	int counter_plus = 1;

	while( counter_plus <= ( M_PI / yaw_res_rad ) + 1 ) {
		
		add_allowed += yaw_res_rad;
		steer_angles.push_back(add_allowed);
		
		//LOG(INFO) << "Allowed steer angle: " << add_allowed;
		counter_plus++;
	}

	add_allowed = 0;
	int counter_minus = 0;
	while( counter_minus < ( M_PI / yaw_res_rad ) - 1 ) {
		
		add_allowed -= yaw_res_rad;
		steer_angles.push_back(add_allowed);
		
		//LOG(INFO) << "Allowed steer angle: " << add_allowed;
		counter_minus++;
	}

	LOG(INFO) << "A total of " << (counter_plus + counter_minus) << " steering angles have been set";


}


int getClosestSteer(vector<float>& steer_angles, float yaw_measured, float& closest ) {

	int closest_index = steer_angles.size();
	float min_dist = FLT_MAX;
	
	for(int i = 0; i < steer_angles.size(); i++) {
		
		float temp_dist = fabs(yaw_measured - steer_angles[i]);
		if( temp_dist < min_dist ) {
			min_dist = temp_dist;
			closest_index = i;
		}

	}

	
	CHECK_LT(closest_index, steer_angles.size()) << "Closest steer not found!";
	closest = steer_angles[closest_index];

	return closest_index;
		
}




} // namespace neural_network_planner


