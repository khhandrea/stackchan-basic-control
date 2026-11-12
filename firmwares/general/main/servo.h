// Feetech SCS0009 serial bus servos (UART1, 1 Mbps): yaw ID 1, pitch ID 2. Power via PY32 pin 0 (VM_EN).
// Methods: servo.get, servo.move, servo.home, servo.power, servo.rotate (not available).
// Event: servo.fault. Stream: servo.
#pragma once

namespace servo {

void init();
void registerMethods();
void start();

}  // namespace servo
