#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------- geometry
#define SCR_W  128
#define SCR_H  128
#define SCR_PX (SCR_W * SCR_H)
#define CX     64.0f
#define CY     64.0f

// ---------------------------------------------------------------- colour
// Native RGB565, red in the high bits -- the format TFT_eSPI sprites store and
// that pushSprite() knows how to send. (An earlier revision pre-swapped these
// into the panel's wire order for a raw DMA push; moving to sprites for text
// rendering made that wrong, so the swap is gone.)
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ---------------------------------------------------------------- fast trig
// A libm sinf() call per pixel would eat the whole frame budget. 1024-entry
// table, masked index, so it also wraps for free and never needs range
// reduction. Negative angles work because two's-complement AND wraps.
#define SIN_BITS 10
#define SIN_N    (1 << SIN_BITS)
#define SIN_MASK (SIN_N - 1)
#define SIN_SCALE (SIN_N / 6.2831853f)

extern float sinTab[SIN_N];

static inline float fsin(float a) {
  return sinTab[((int)(a * SIN_SCALE)) & SIN_MASK];
}
static inline float fcos(float a) {
  return sinTab[(((int)(a * SIN_SCALE)) + (SIN_N / 4)) & SIN_MASK];
}

// Distance from screen centre, precomputed. Max is sqrt(64^2+64^2) = 90.5,
// so a byte per pixel is plenty and it kills the per-pixel sqrtf().
extern uint8_t radTab[SCR_PX];

// ---------------------------------------------------------------- palette
#define PAL_N 256
extern uint16_t pal[PAL_N];

struct Pal3 { float r, g, b; };

// Inigo Quilez cosine palette:  c(t) = a + b * cos(2*pi*(c*t + d))
// Cheap to build, and every entry lands pre-byte-swapped for the panel.
//
// Pastels come from the a/b terms, not the hues: a high baseline (~0.75) with
// a small amplitude (~0.15) keeps every channel in the top half of its range,
// so nothing ever reaches full saturation or goes dark. Drop a to 0.5 and
// raise b to 0.5 and you get the usual saturated rainbow back.
void buildPalette(Pal3 a, Pal3 b, Pal3 c, Pal3 d);

struct Palette { const char* name; Pal3 a, b, c, d; };
extern const Palette palettes[];
extern const int     paletteCount;
void applyPalette(int i);

// ---------------------------------------------------------------- misc
void  gfxInit();                                   // sinTab + radTab
void  drawNum(uint16_t* fb, int x, int y, int val, uint16_t col, int scale);
