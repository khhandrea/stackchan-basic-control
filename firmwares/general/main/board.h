// Core unit board setup: AW9523 IO expander, panel reset, FT6336 touch controller handle.
#pragma once

#include "i2c_bus.h"

namespace board {

enum class Panel { Ili9342c, Ili9342e };

void init();
// Pulses the LCD reset line (AW9523 P1.1).
void resetLcd();
// Detected from the FT6336 version (same rule as the official firmware).
Panel panel();
i2c::Device& ft6336();

}  // namespace board
