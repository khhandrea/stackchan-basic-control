// Display task: renders the face and modal at up to 30 fps.
// Methods: display.set_brightness, display.power.
#pragma once

namespace display {

bool init();
void registerMethods();
void start();
// Idle overrides (restore the set brightness / power when cleared)
void setIdleDim(bool on);        // backlight at most 64 / 255
void setIdlePowerSave(bool on);  // panel and backlight off

}  // namespace display
