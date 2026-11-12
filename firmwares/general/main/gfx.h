// Band renderer: shapes are rasterized into one horizontal band of the screen at a time.
// Pixels are RGB565 stored big-endian (ready for the LCD).
#pragma once

#include <cstdint>

namespace gfx {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return static_cast<uint16_t>((c >> 8) | (c << 8));
}

struct Band {
    uint16_t* px;
    int width;
    int y0;  // first screen row in this band
    int rows;
};

void fill(const Band& b, uint16_t color);
void fillRect(const Band& b, int x, int y, int w, int h, uint16_t color);
void strokeRect(const Band& b, int x, int y, int w, int h, int t, uint16_t color);

// Rotated ellipse; angle in degrees.
void fillEllipse(const Band& b, float cx, float cy, float rx, float ry, float angle, uint16_t color);
// Rotated rounded rectangle (w x h, corner radius r).
void fillRoundRect(const Band& b, float cx, float cy, float w, float h, float r, float angle, uint16_t color);
void fillRing(const Band& b, float cx, float cy, float r_outer, float r_inner, uint16_t color);
void fillHeart(const Band& b, float cx, float cy, float size, uint16_t color);
// Water drop: round bottom of radius r, pointed top.
void fillDrop(const Band& b, float cx, float cy, float r, uint16_t color);

// Generic shape: inside(x, y) decides each pixel in the bounding box.
template <typename Inside>
void fillShape(const Band& b, int x0, int y0, int x1, int y1, uint16_t color, Inside inside) {
    if (y0 < b.y0) y0 = b.y0;
    if (y1 > b.y0 + b.rows - 1) y1 = b.y0 + b.rows - 1;
    if (x0 < 0) x0 = 0;
    if (x1 > b.width - 1) x1 = b.width - 1;
    for (int y = y0; y <= y1; y++) {
        uint16_t* row = b.px + (y - b.y0) * b.width;
        for (int x = x0; x <= x1; x++) {
            if (inside(x + 0.5f, y + 0.5f)) {
                row[x] = color;
            }
        }
    }
}

// ---- text (16 px bitmap font: ASCII 8x16, Hangul 16x16) ----

constexpr int kFontHeight = 16;

// Decodes one UTF-8 code point and advances *s. Returns 0 at end of string.
uint32_t nextCodepoint(const char** s);
int glyphWidth(uint32_t cp);
int textWidth(const char* utf8);
// Draws one line of text with its top-left corner at (x, y); bold draws twice with a 1 px offset.
void drawText(const Band& b, int x, int y, const char* utf8, uint16_t color, bool bold = false);
// Draws n code points starting at utf8 (used for wrapped lines).
void drawTextN(const Band& b, int x, int y, const char* utf8, int n, uint16_t color, bool bold = false);

}  // namespace gfx
