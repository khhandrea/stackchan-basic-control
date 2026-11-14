// 12 RGB LEDs (WS2812C) driven by the PY32L020 IO expander: 0-5 left, 6-11 right.
// Methods: led.get, led.set_all, led.set_side, led.set_one, led.set_brightness.
#pragma once

namespace led {

void init();
void registerMethods();
void start();
// Idle power save: all LEDs off except LED 0 blinking red (20 %, 2 s on / 2 s off); clearing restores the LEDs.
void setPowerSave(bool on);

}  // namespace led
