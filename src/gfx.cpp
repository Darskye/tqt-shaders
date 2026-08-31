#include "gfx.h"
#include <math.h>

float   sinTab[SIN_N];
uint8_t radTab[SCR_PX];
uint16_t pal[PAL_N];

void gfxInit() {
  for (int i = 0; i < SIN_N; i++)
    sinTab[i] = sinf((float)i * 6.2831853f / (float)SIN_N);

  for (int y = 0; y < SCR_H; y++) {
    for (int x = 0; x < SCR_W; x++) {
      float dx = (float)x - CX;
      float dy = (float)y - CY;
      radTab[y * SCR_W + x] = (uint8_t)(sqrtf(dx * dx + dy * dy) + 0.5f);
    }
  }
}

void buildPalette(Pal3 a, Pal3 b, Pal3 c, Pal3 d) {
  for (int i = 0; i < PAL_N; i++) {
    float t = (float)i / (float)(PAL_N - 1);
    float r = a.r + b.r * cosf(6.2831853f * (c.r * t + d.r));
    float g = a.g + b.g * cosf(6.2831853f * (c.g * t + d.g));
    float bl = a.b + b.b * cosf(6.2831853f * (c.b * t + d.b));
    if (r  < 0) r  = 0; if (r  > 1) r  = 1;
    if (g  < 0) g  = 0; if (g  > 1) g  = 1;
    if (bl < 0) bl = 0; if (bl > 1) bl = 1;
    pal[i] = rgb565((uint8_t)(r * 255.0f), (uint8_t)(g * 255.0f), (uint8_t)(bl * 255.0f));
  }
}

// ---------------------------------------------------------------- 3x5 digits
// Just enough font to put an FPS number on the panel without dragging in
// TFT_eSPI's text engine, which draws to the display and not to our buffer.
static const uint8_t glyph[10][5] = {
  {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
  {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7}
};

static void drawGlyph(uint16_t* fb, int gx, int gy, int d, uint16_t col, int s) {
  for (int row = 0; row < 5; row++) {
    uint8_t bits = glyph[d][row];
    for (int bit = 0; bit < 3; bit++) {
      if (!(bits & (4 >> bit))) continue;
      for (int sy = 0; sy < s; sy++) {
        int py = gy + row * s + sy;
        if (py < 0 || py >= SCR_H) continue;
        uint16_t* p = fb + py * SCR_W;
        for (int sx = 0; sx < s; sx++) {
          int px = gx + bit * s + sx;
          if (px >= 0 && px < SCR_W) p[px] = col;
        }
      }
    }
  }
}

void drawNum(uint16_t* fb, int x, int y, int val, uint16_t col, int scale) {
  if (val < 0) val = 0;
  int digits[6], n = 0;
  do { digits[n++] = val % 10; val /= 10; } while (val && n < 6);
  for (int i = 0; i < n; i++)
    drawGlyph(fb, x + (n - 1 - i) * 4 * scale, y, digits[i], col, scale);
}

// ---------------------------------------------------------------- palettes
// Lo-fi pastels. Every channel stays between roughly 0.5 and 0.97, which is
// what stops them reading as neon: no channel bottoms out, no channel pins.
const Palette palettes[] = {
  // name        a (baseline)            b (amplitude)           c (cycles)              d (phase)
  {"cotton",   {0.80f,0.70f,0.80f}, {0.15f,0.18f,0.15f}, {1.0f,1.0f,1.0f}, {0.00f,0.25f,0.50f}},
  {"sorbet",   {0.85f,0.74f,0.70f}, {0.12f,0.15f,0.16f}, {1.0f,1.0f,1.0f}, {0.00f,0.12f,0.28f}},
  {"seafoam",  {0.72f,0.84f,0.80f}, {0.18f,0.12f,0.14f}, {1.0f,1.0f,1.0f}, {0.35f,0.05f,0.18f}},
  {"dusk",     {0.70f,0.64f,0.78f}, {0.18f,0.16f,0.14f}, {1.0f,1.0f,1.0f}, {0.08f,0.32f,0.58f}},
  {"matcha",   {0.76f,0.82f,0.68f}, {0.14f,0.12f,0.16f}, {1.0f,1.0f,1.0f}, {0.20f,0.10f,0.40f}},
  {"faded",    {0.76f,0.72f,0.74f}, {0.10f,0.10f,0.12f}, {1.0f,1.0f,1.0f}, {0.00f,0.20f,0.40f}},
};
const int paletteCount = sizeof(palettes) / sizeof(palettes[0]);

void applyPalette(int i) {
  const Palette& q = palettes[i];
  buildPalette(q.a, q.b, q.c, q.d);
}
