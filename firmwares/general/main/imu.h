// IMU: BMI270 accelerometer / gyroscope, BMM150 magnetometer (BMI270 aux bus), motion detection.
// Method: imu.get. Event: motion. Stream: imu.
#pragma once

namespace imu {

void init();
void registerMethods();
void start();

}  // namespace imu
