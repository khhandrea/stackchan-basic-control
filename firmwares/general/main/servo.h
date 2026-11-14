// Feetech SCS0009 serial bus servos (UART1, 1 Mbps): yaw ID 1, pitch ID 2. Power via PY32 pin 0 (VM_EN).
// Powered on automatically at boot (holding the current position) and after idle; off while idle.
// Methods: servo.get, servo.move, servo.home, servo.power, servo.rotate (not available).
// Event: servo.fault. Stream: servo.
#pragma once

namespace servo {

void init();
void registerMethods();
void start();

// Reaction "lift": raise the pitch a little from its current target (only when powered; never powers on).
// Clearing returns to the previous target unless someone moved the servo meanwhile.
void reactLift(bool on);
// True while a commanded move is still in progress
bool moving();
// Idle: cut servo power while on (sleepy stage and deeper); restored on wake if still wanted.
void setIdleOff(bool off);

}  // namespace servo
