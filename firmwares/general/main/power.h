// Power: AXP2101 PMIC, INA226 battery monitor, chip temperature, power button.
// Methods: sys.status, sys.power_off, power.set_charge_mode, button.get, log.set_level, log.set_power_interval.
// Events: power.usb, power.charge, power.low_battery, button.
#pragma once

#include <cstdint>

namespace power {

void init();
void registerMethods();
// Registers polling and the "status" stream. Call after init().
void start();

// LCD backlight (AXP2101 DLDO1): 0 = off, 1-100 = brightness.
void setBacklight(int percent);
// True once per power button press (since the last call); for the reactions.
bool takeButtonPress();

}  // namespace power
