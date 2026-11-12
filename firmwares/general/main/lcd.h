// ILI9342C/E 320x240 LCD on SPI3 (CS = G3, DC = G35), pushed in horizontal bands.
#pragma once

#include <cstdint>

namespace lcd {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kBandHeight = 24;  // 10 bands per frame

bool init();
bool ready();

// Returns a free band buffer (kWidth * kBandHeight pixels, RGB565 big-endian), waiting if both are in flight.
uint16_t* acquireBand();
// Queues the band at rows y0 .. y0 + rows - 1 for transfer (DMA); the buffer is released when done.
void pushBand(uint16_t* band, int y0, int rows);

void setOn(bool on);

}  // namespace lcd
