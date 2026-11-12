#include "gfx.h"

#include <cmath>

namespace gfx {
namespace {

constexpr float kDegToRad = 3.14159265f / 180.0f;

}  // namespace

void fill(const Band& b, uint16_t color) {
    for (int i = 0; i < b.width * b.rows; i++) {
        b.px[i] = color;
    }
}

void fillRect(const Band& b, int x, int y, int w, int h, uint16_t color) {
    fillShape(b, x, y, x + w - 1, y + h - 1, color, [](float, float) { return true; });
}

void strokeRect(const Band& b, int x, int y, int w, int h, int t, uint16_t color) {
    fillRect(b, x, y, w, t, color);
    fillRect(b, x, y + h - t, w, t, color);
    fillRect(b, x, y, t, h, color);
    fillRect(b, x + w - t, y, t, h, color);
}

void fillEllipse(const Band& b, float cx, float cy, float rx, float ry, float angle, uint16_t color) {
    if (rx <= 0 || ry <= 0) {
        return;
    }
    float c = cosf(angle * kDegToRad);
    float s = sinf(angle * kDegToRad);
    float r = rx > ry ? rx : ry;
    fillShape(b, static_cast<int>(cx - r - 1), static_cast<int>(cy - r - 1), static_cast<int>(cx + r + 1),
              static_cast<int>(cy + r + 1), color, [=](float x, float y) {
                  float dx = x - cx;
                  float dy = y - cy;
                  float u = (dx * c + dy * s) / rx;
                  float v = (-dx * s + dy * c) / ry;
                  return u * u + v * v <= 1.0f;
              });
}

void fillRoundRect(const Band& b, float cx, float cy, float w, float h, float r, float angle, uint16_t color) {
    float c = cosf(angle * kDegToRad);
    float s = sinf(angle * kDegToRad);
    float hw = w / 2;
    float hh = h / 2;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    float reach = sqrtf(hw * hw + hh * hh) + 1;
    fillShape(b, static_cast<int>(cx - reach), static_cast<int>(cy - reach), static_cast<int>(cx + reach),
              static_cast<int>(cy + reach), color, [=](float x, float y) {
                  float dx = x - cx;
                  float dy = y - cy;
                  float u = fabsf(dx * c + dy * s) - (hw - r);
                  float v = fabsf(-dx * s + dy * c) - (hh - r);
                  float ou = u > 0 ? u : 0;
                  float ov = v > 0 ? v : 0;
                  return ou * ou + ov * ov <= r * r && (u <= r && v <= r);
              });
}

void fillRing(const Band& b, float cx, float cy, float r_outer, float r_inner, uint16_t color) {
    fillShape(b, static_cast<int>(cx - r_outer - 1), static_cast<int>(cy - r_outer - 1),
              static_cast<int>(cx + r_outer + 1), static_cast<int>(cy + r_outer + 1), color, [=](float x, float y) {
                  float d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                  return d <= r_outer * r_outer && d >= r_inner * r_inner;
              });
}

void fillHeart(const Band& b, float cx, float cy, float size, uint16_t color) {
    float k = 1.3f / (size / 2);  // map the half size to ~1.3 heart units
    fillShape(b, static_cast<int>(cx - size), static_cast<int>(cy - size), static_cast<int>(cx + size),
              static_cast<int>(cy + size), color, [=](float x, float y) {
                  float u = (x - cx) * k;
                  float v = -(y - cy) * k + 0.2f;
                  float a = u * u + v * v - 1;
                  return a * a * a - u * u * v * v * v <= 0;
              });
}

void fillDrop(const Band& b, float cx, float cy, float r, uint16_t color) {
    // Circle at (cx, cy) plus a triangle up to the tip at (cx, cy - 2.2 r)
    float tip = cy - 2.2f * r;
    fillShape(b, static_cast<int>(cx - r - 1), static_cast<int>(tip - 1), static_cast<int>(cx + r + 1),
              static_cast<int>(cy + r + 1), color, [=](float x, float y) {
                  float dx = x - cx;
                  float dy = y - cy;
                  if (dx * dx + dy * dy <= r * r) {
                      return true;
                  }
                  if (y < tip || y > cy) {
                      return false;
                  }
                  float half = r * 0.92f * (y - tip) / (cy - tip);
                  return fabsf(dx) <= half;
              });
}

// ---- text ----

extern "C" const uint8_t font16_start[] asm("_binary_font16_fnt_start");

namespace {

constexpr uint32_t kAsciiFirst = 0x20, kAsciiLast = 0x7E;
constexpr uint32_t kJamoFirst = 0x3131, kJamoLast = 0x318E;
constexpr uint32_t kSyllableFirst = 0xAC00, kSyllableLast = 0xD7A3;
constexpr size_t kAsciiBytes = 16, kWideBytes = 32;
constexpr size_t kJamoOffset = (kAsciiLast - kAsciiFirst + 1) * kAsciiBytes;
constexpr size_t kSyllableOffset = kJamoOffset + (kJamoLast - kJamoFirst + 1) * kWideBytes;

// Returns the glyph bitmap; *wide is set for 16 px glyphs. Unknown code points map to '?'.
const uint8_t* glyph(uint32_t cp, bool* wide) {
    *wide = true;
    if (cp >= kSyllableFirst && cp <= kSyllableLast) {
        return font16_start + kSyllableOffset + (cp - kSyllableFirst) * kWideBytes;
    }
    if (cp >= kJamoFirst && cp <= kJamoLast) {
        return font16_start + kJamoOffset + (cp - kJamoFirst) * kWideBytes;
    }
    *wide = false;
    if (cp < kAsciiFirst || cp > kAsciiLast) {
        cp = '?';
    }
    return font16_start + (cp - kAsciiFirst) * kAsciiBytes;
}

void drawGlyph(const Band& b, int x, int y, uint32_t cp, uint16_t color) {
    bool wide;
    const uint8_t* g = glyph(cp, &wide);
    int w = wide ? 16 : 8;
    for (int row = 0; row < kFontHeight; row++) {
        int sy = y + row;
        if (sy < b.y0 || sy >= b.y0 + b.rows) {
            continue;
        }
        uint16_t bits = wide ? static_cast<uint16_t>((g[row * 2] << 8) | g[row * 2 + 1])
                             : static_cast<uint16_t>(g[row] << 8);
        uint16_t* line = b.px + (sy - b.y0) * b.width;
        for (int col = 0; col < w; col++) {
            int sx = x + col;
            if ((bits & (0x8000 >> col)) && sx >= 0 && sx < b.width) {
                line[sx] = color;
            }
        }
    }
}

}  // namespace

uint32_t nextCodepoint(const char** s) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(*s);
    uint32_t cp = p[0];
    int len = 1;
    if (cp == 0) {
        return 0;
    } else if ((cp & 0xE0) == 0xC0 && p[1]) {
        cp = ((cp & 0x1F) << 6) | (p[1] & 0x3F);
        len = 2;
    } else if ((cp & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = ((cp & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        len = 3;
    } else if ((cp & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        cp = ((cp & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        len = 4;
    } else if (cp >= 0x80) {
        cp = '?';
    }
    *s += len;
    return cp;
}

int glyphWidth(uint32_t cp) {
    bool wide;
    glyph(cp, &wide);
    return wide ? 16 : 8;
}

int textWidth(const char* s) {
    int w = 0;
    while (uint32_t cp = nextCodepoint(&s)) {
        w += glyphWidth(cp);
    }
    return w;
}

void drawTextN(const Band& b, int x, int y, const char* s, int n, uint16_t color, bool bold) {
    if (y + kFontHeight <= b.y0 || y >= b.y0 + b.rows) {
        return;
    }
    while (n-- > 0) {
        uint32_t cp = nextCodepoint(&s);
        if (cp == 0) {
            break;
        }
        drawGlyph(b, x, y, cp, color);
        if (bold) {
            drawGlyph(b, x + 1, y, cp, color);
        }
        x += glyphWidth(cp);
    }
}

void drawText(const Band& b, int x, int y, const char* s, uint16_t color, bool bold) {
    drawTextN(b, x, y, s, 1 << 30, color, bold);
}

}  // namespace gfx
