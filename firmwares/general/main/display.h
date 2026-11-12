// Display task: renders the face and modal at up to 30 fps.
// Methods: display.set_brightness, display.power.
#pragma once

namespace display {

bool init();
void registerMethods();
void start();

}  // namespace display
