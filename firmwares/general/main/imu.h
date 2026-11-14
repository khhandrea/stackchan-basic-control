// IMU: BMI270 accelerometer / gyroscope, BMM150 magnetometer (BMI270 aux bus), motion detection.
// Method: imu.get. Event: motion. Stream: imu.
#pragma once

#include <cstdint>

namespace imu {

void init();
void registerMethods();
void start();

enum class Motion { Still, Moving, Tilted, Fallen };
Motion motion();
// Incremented on every "shaken" detection
uint32_t shakeCount();
// Incremented on every "nudged" (light shake) detection
uint32_t nudgeCount();

}  // namespace imu
