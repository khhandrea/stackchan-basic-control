// Reactions (face.set_auto "react"), shown as an overlay on the set face:
// - Head touch: happy + shy, mouth a little open (half moon), and a slight head lift (only if servo power is on); back 5 s after the last touch
// - Shaken hard: unhappy + sweat; back after 5 s without moving
// - IMU "moving" (not caused by our own servos), or a power button press: neutral + question mark;
//   back after 3 s without moving
#pragma once

namespace react {

void start();

}  // namespace react
