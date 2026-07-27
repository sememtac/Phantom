#pragma once
#include <stdint.h>

// 5x7 bitmap font, ASCII 32..90 (space through 'Z'). Column-major, bit 0 is the
// top row. Lowercase is folded to uppercase by the caller; there is no bold,
// because in this interface hierarchy is size and intensity, never weight.
#define VG_FONT_FIRST 32
#define VG_FONT_LAST  90
#define VG_FONT_COUNT (VG_FONT_LAST - VG_FONT_FIRST + 1)

extern const uint8_t VG_FONT5X7[VG_FONT_COUNT][5];
