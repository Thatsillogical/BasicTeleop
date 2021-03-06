//Imports
#include "ros/ros.h"
#include "std_msgs/String.h"
#include <sensor_msgs/Image.h>
#include <sstream>
#include "basic_teleop/Move.h"
#include "limits.h"
#include <typeinfo>
#include "sensor_msgs/CameraInfo.h"
#include <cmath>
#include <boost/date_time.hpp>
#include <array>

//Constants
#define WIDTH 320
#define HEIGHT 240
#define PRINT_NAME(x) std::cout << #x << " - " << typeid(x).name() << '\n' // For Debugging
#define DEGREES_PER_SECOND 90
#define X_SHIFT .16
#define BLOB_TOLERANCE 15
#define BLOB_MIN_SIZE 25
#define HORIZON 240

//Namespaces
namespace btt = boost::posix_time;


//Types and Classes
union depth_data { //For processing data as mm values
    unsigned char raw[WIDTH * HEIGHT * 2]; //uint8 counterpart
    unsigned short int mm[WIDTH * HEIGHT]; //unit16 counterpart
};

class Blob {
    public:
        Blob () : topX(0), topY(0), botX(0), botY(0) {};
        Blob (int x, int y, int x2, int y2) : topX(x), topY(y), botX(x2), botY(y2) {};
        Blob (const depth_data&, int, int);        
        Blob (const depth_data&);        
        int getArea();
        std::array<int, 2> getCenter();
        bool inBlob(const std::array<int, 2>);
    private:
        int topX;
        int topY;
        int botX;
        int botY;
};
    
//Globals
sensor_msgs::CameraInfo camera;
basic_teleop::Move last_dir;
Blob last_blob;
std::array<float, 3> last_untransformed;
std::array<float, 3> velocity; // m/s
btt::ptime last_time = btt::microsec_clock::universal_time();
bool received_first = false;
bool received_2 = false;
bool calibrate_lag = false;
depth_data d;

//Protofunctions
int getXY(const depth_data&, int, int);
void print_closest_point_pixel_coords(const depth_data&);
std::array<float, 3> conv_to_3d(const depth_data&, int, int, const double*);
std::array<int, 2> conv_to_2d(const std::array<float, 3>, const double*);
void change_velocity(const std::array<float, 3>);

//Definitions
Blob::Blob (const depth_data& grid, int x, int y) {
    int depth_standard = getXY(grid, x, y);
    topX = x;
    botX = x;
    topY = y;
    botY = y;

    while (true){ // search for right of blob;
        int cur_reading = getXY(grid, botX, y); // Bring statement out of while 
                                                   // loop to save calc time.

        if (!((cur_reading <= (depth_standard + BLOB_TOLERANCE))
            && (cur_reading >= (depth_standard - BLOB_TOLERANCE))
            && (botX < WIDTH - 1))) {
            break;
        }

        depth_standard = cur_reading; //follow smooth enough slopes
        botX++;
        
    }
    
    depth_standard = getXY(grid, x, y); // reset standard to center;

    while (true){ // search for bottom of blob;
        int cur_reading = getXY(grid, x, botY); // Bring statement out of while 
                                                   // loop to save calc time.

        if (!((cur_reading <= (depth_standard + BLOB_TOLERANCE))
            && (cur_reading >= (depth_standard - BLOB_TOLERANCE))
            && (botY < HEIGHT - 1))) {
            break;
        }
        
        depth_standard = cur_reading;
        botY++;
        
    }
    
    depth_standard = getXY(grid, x, y); // reset standard to center;
    
    while (true){ // search for left of blob;
        int cur_reading = getXY(grid, topX, y); // Bring statement out of while 
                                                   // loop to save calc time.

        if (!((cur_reading <= (depth_standard + BLOB_TOLERANCE))
            && (cur_reading >= (depth_standard - BLOB_TOLERANCE))
            && (topX > 1))) {
            break;
        }
        
        depth_standard = cur_reading;
        topX--;
        
    }
    
    while (true){ // search for left of blob;
        int cur_reading = getXY(grid, x, topY); // Bring statement out of while 
                                                   // loop to save calc time.

        if (!((cur_reading <= (depth_standard + BLOB_TOLERANCE))
            && (cur_reading >= (depth_standard - BLOB_TOLERANCE))
            && (topY > 1))) {
            break;
        }
        
        depth_standard = cur_reading;
        topY--;
        
    }
    
}

Blob::Blob (const depth_data& grid) {
    Blob b = Blob();
    int too_close = 0;
    while(b.getArea() < BLOB_MIN_SIZE) {
        int new_x, new_y;
        int closest = INT_MAX;
        for(int x = 0; x < WIDTH; x++){
            for(int y = 0; y < HEIGHT; y++){
                if ((getXY(grid, x, y) < closest) && (getXY(grid, x, y) > too_close)){
                    new_x = x;
                    new_y = y;
                    closest = getXY(grid, x, y);
                }
            }
        }
        //ROS_INFO("Working on : [%d, %d]", new_x, new_y);
        b = Blob(grid, new_x, new_y);
        too_close = closest;
    }
    topX = b.topX;
    topY = b.topY;
    botX = b.botX;
    botY = b.botY;
}

int Blob::getArea() {
    /*ROS_INFO("botX: [%d]", botX);
    ROS_INFO("topX: [%d]", topX);
    ROS_INFO("botY: [%d]", botY);
    ROS_INFO("topY: [%d]", topY);*/
    return (botX - topX) * (botY - topY);
}

std::array<int, 2> Blob::getCenter() {
    /*ROS_INFO("botX: [%d]", botX);
    ROS_INFO("topX: [%d]", topX);
    ROS_INFO("botY: [%d]", botY);
    ROS_INFO("topY: [%d]", topY);*/
    return std::array<int, 2>{ //was this broken?
                        {(topX + ((botX - topX) / 2)), 
                         (topY + ((botY - topY) / 2))}
                        };
}

bool Blob::inBlob(const std::array<int, 2> arr) {
    return ((topX - 5 <= arr[0]) && (arr[0] + 5 <= botX)
            && (topY - 5 <= arr[1]) && (arr[1] + 5 <= botY));
}

void callback(sensor_msgs::Image frame) {  
    depth_data grid;
    memcpy(&grid, frame.data.data(), sizeof(depth_data));
    d = grid;
    Blob b = Blob(grid);    
    
    std::array<int, 2> center = b.getCenter();
    std::array<float, 3> real_coords = conv_to_3d(grid, center[0], center[1], camera.K.data()); //may need to convert camera data
    
    /*int backwards[2] = {0, 0};
    conv_to_2d(real_coords, camera.K.data(), backwards);

    */ROS_INFO("Center At: [%d, %d]", center[0], center[1]); //For Debugging
    /*ROS_INFO("Area: [%d]", b.getArea());*/
    ROS_INFO("Real World Last (x,y,z): [%f, %f, %f]", last_untransformed[0], last_untransformed[1], last_untransformed[2]);
    ROS_INFO("Real World Real (x,y,z): [%f, %f, %f]", real_coords[0], real_coords[1], real_coords[2]);

    if (received_first) { // make sure we have real starting point to compare against    
        change_velocity(real_coords);
    }
    
    last_untransformed[0] = real_coords[0];
    last_untransformed[1] = real_coords[1];
    last_untransformed[2] = real_coords[2];    
    last_time = btt::microsec_clock::universal_time();
    last_blob = b;

    if (!received_first) {
        received_first = true;   
    }
    else if (!received_2) {
        received_2 = true;
    }
}

inline int getXY(const depth_data& grid, int x, int y) {
    return grid.mm[x + (y * WIDTH)];
}

void set_camera_info(sensor_msgs::CameraInfo c){
    camera = c;
}

std::array<float, 3> conv_to_3d(const depth_data& grid, int x, int y, const double* intr){
    //using default values found online    
    float cx = 319.5;// intr[2];
    float cy = 239.5;// intr[5];
    float fx_inv = 1.0 / 525.0;// intr[0];
    float fy_inv = 1.0 / 525.0;// intr[4];

    float zO = getXY(grid, x, y) * .001; // z
    float xO = (zO * ((x - cx) * fx_inv)) + X_SHIFT; //x
    float yO = zO * ((y - cy) * fy_inv); //y

    return std::array<float, 3> { {xO, yO, zO} };
}

std::array<int, 2> conv_to_2d(const std::array<float, 3> in, const double* intr){
    //using default values found online    
    float cx = 319.5;// intr[2];
    float cy = 239.5;// intr[5];
    float fx = 525.0;// intr[0];
    float fy = 525.0;// intr[4];    

    return std::array<int, 2> { 
                              {(((in[0] - X_SHIFT) * fx) / in[2]) + cx,
                               ((in[1] * fy) / in[2]) + cy}
                              };
}

std::array<float, 3> rotate_point_around_robot(const std::array<float, 3> in, float angle) {
    float x = in[0];// height doesn't change with turning
    float z = in[2];

    angle = angle*M_PI/180; // Convert to radians
    
    float pivX = 0; //point around center of robot
    float pivZ = -.06;

    x -= pivX;
    z -= pivZ;

    x = x*cos(angle) - z*sin(angle);
    z = z*cos(angle) + x*sin(angle);

    x += pivX;
    z += pivZ;

    return std::array<float, 3> { {x, in[1], z} }; //care for pointer problems
}

void change_velocity(const std::array<float, 3> new_transformed) {
    double time_passed = (btt::microsec_clock::universal_time() - last_time).total_milliseconds() / 1000.0; //In Seconds 
    float angle_turned = DEGREES_PER_SECOND * time_passed;
    if (last_dir.direction == "ccw") {
        angle_turned *= -1.0; // is this right for undoing the turning? - since cw is in the negative direction, flip 
    }
    else if (last_dir.direction == "fwd") {
        angle_turned = 0;
    }

    std::array<float, 3> temp = rotate_point_around_robot(new_transformed, angle_turned); // undo expected rotation

    if (last_blob.inBlob(conv_to_2d(temp, camera.K.data()))) {
        velocity[0] = 0;
        velocity[1] = 0;
        velocity[2] = 0;
    }
    else {
        velocity[0] = (temp[0] - last_untransformed[0]) / time_passed;
        velocity[1] = (temp[1] - last_untransformed[1]) / time_passed;
        velocity[2] = (temp[2] - last_untransformed[2]) / time_passed;
    }
    ROS_INFO("Temp[0]: %f", temp[0]);
    ROS_INFO("last_untransf[0]: %f", last_untransformed[0]);
    ROS_INFO("Time passed: %f", time_passed);
    ROS_INFO("New Vel (x,y,z): [%f, %f, %f]", velocity[0], velocity[1], velocity[2]);
}

std::array<float, 3> predict_new_center() {
    double time_passed = (btt::microsec_clock::universal_time() - last_time).total_milliseconds() / 1000.0; //In Seconds   
    std::array<float, 3> new_center { {0.0, 0.0, 0.0} };
    new_center[0] = last_untransformed[0] + (velocity[0] * time_passed); //apply predicted movement 
                                                                         //of target
    new_center[1] = last_untransformed[1] + (velocity[1] * time_passed);
    new_center[2] = last_untransformed[2] + (velocity[2] * time_passed); 

    float angle_turned = DEGREES_PER_SECOND * time_passed;
    
    if (last_dir.direction.data() == "cw") {
        angle_turned *= -1.0;
    }

    return rotate_point_around_robot(new_center, angle_turned); // apply predicted movement of robot
}

void print_closest_point_pixel_coords(const depth_data& grid) {
    int out_x, out_y;
    int farthest = INT_MAX; 
    for(int x = 0; x < WIDTH; x++){
        for(int y = 0; y < HEIGHT; y++){
            if ((getXY(grid, x, y) < farthest) && (getXY(grid, x, y) != 0)){
                out_x = x;
                out_y = y;
                farthest = getXY(grid, x, y);
            }
        }
    }
    ROS_INFO("I heard: [%d, %d]", out_x, out_y);
}

void print_type_shortcuts() {
    PRINT_NAME(char);
    PRINT_NAME(signed char);
    PRINT_NAME(unsigned char);
    PRINT_NAME(short);
    PRINT_NAME(unsigned short);
    PRINT_NAME(int);
    PRINT_NAME(unsigned int);
    PRINT_NAME(long);
    PRINT_NAME(unsigned long);
    PRINT_NAME(float);
    PRINT_NAME(double);
    PRINT_NAME(long double);
    PRINT_NAME(char*);
    PRINT_NAME(const char*);
}

/**
 * This tutorial demonstrates simple sending of messages over the ROS system.
 */
int main(int argc, char **argv)
{

  ros::init(argc, argv, "predictive_director");

  ros::NodeHandle n;

  ros::Subscriber temp = n.subscribe("/openni2_camera/depth/camera_info", 1000, set_camera_info);

  while(&camera == NULL) {//wait until we have camera information
    continue;  
  }

  temp.~Subscriber();

  ros::Subscriber sub = n.subscribe("/openni2_camera/depth/image_raw", 1000, callback);
  ros::Publisher dir_pub = n.advertise<basic_teleop::Move>("movement", 1000);

  ros::Rate loop_rate(10); 

  /**
   * A count of how many messages we have sent.
   */
  int count = 0;
    
  last_dir.direction = "fwd";

  while (ros::ok())
  {
    
    while(!received_2) { // wait until we have valid velocity data
        ros::spinOnce();

        loop_rate.sleep();
    } 

    /*while(calibrate_lag && ros::ok()) {
        std::array<float, 3> first_center = last_untransformed;
        basic_teleop::Move next;
        next.direction = "cw";
        dir_pub.publish(next);
        ros::Rate loop_rate1(2);
        loop_rate1.sleep();
        next.direction = "fwd";
        dir_pub.publish(next);  //drive for half of a second
        
        ros::Rate loop_rate2(1000);
        int millicount = 0;
        !last_blob.inBlob(conv_to_2d(
                                        rotate_point_around_robot(
                                            first_center
                                        , 45)
                                , camera.K.data())) 
        while (millicount < 500 && ros::ok()) { // while the non moving blob hasn't reached it's predicted position
            ros::spinOnce();
            millicount++;
            loop_rate2.sleep();
            ROS_INFO("First Blob Center: [%d, %d] New Blob Center: [%d, %d] and %d", conv_to_2d(first_center, camera.K.data())[0], conv_to_2d(first_center, camera.K.data())[1], last_blob.getCenter()[0], last_blob.getCenter()[1], millicount);
        }

        ROS_INFO("Estimated picture lag: [%d]", millicount);
        
    }*/ // This ended up being too error prone - will calculate by hand a picture lag...

    ROS_INFO("Last pub-ed: [%s]", last_dir.direction.data());  

    std::array<float, 3> new_center = predict_new_center();    

    basic_teleop::Move next;

    std::array<int, 2> temp_2d = conv_to_2d(new_center, camera.K.data());

    if(temp_2d[0] >= 130 && temp_2d[0] <= 190) {
        next.direction = "fwd";
    }
    else if(temp_2d[0] < 130){ // to the right of the robot is lower values
        next.direction = "cw";
    }
    else {
        next.direction = "ccw";
    }
    
    dir_pub.publish(next);
    last_dir = next;  
    
    ros::spinOnce();

    loop_rate.sleep();
    ++count;
  }
    
  return 0;
}
