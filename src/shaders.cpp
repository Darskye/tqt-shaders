#include "shaders.h"
#include "gfx.h"

// ============================================================== plasma
// Four summed sine fields, one of them radial. The classic. Cheap enough
// that it should peg whatever the SPI bus will take.

static void plasmaRender(uint16_t* fb, int y0, int y1, float t) {
  for (int y = y0; y < y1; y++) {
    float fy = (float)y;
    float rowA = fsin(fy * 0.070f - t * 0.80f);   // constant across the row
    float rowB = fy * 0.045f + t * 1.30f;
    const uint8_t* rad = radTab + y * SCR_W;
    uint16_t* p = fb + y * SCR_W;
    for (int x = 0; x < SCR_W; x++) {
      float fx = (float)x;
      float v = rowA
              + fsin(fx * 0.060f + t)
              + fsin(fx * 0.045f + rowB)
              + fsin((float)rad[x] * 0.110f - t * 2.0f);
      // v is in [-4,4]; the cosine palette is cyclic so the mask wraps cleanly
      p[x] = pal[((int)((v + 4.0f) * 31.9f)) & 255];
    }
  }
}

// ============================================================== metaballs
// Five blobs on lissajous paths. Note the falloff is a clamped quartic
// rather than the textbook r^2/d^2 -- that avoids a per-ball divide, which
// on this FPU is the difference between comfortable and not.

#define NB 5

static void metaRender(uint16_t* fb, int y0, int y1, float t) {
  static const float sx[NB]  = {0.73f, 0.51f, 0.94f, 0.37f, 1.11f};
  static const float sy[NB]  = {0.61f, 0.87f, 0.43f, 1.03f, 0.55f};
  static const float ph[NB]  = {0.00f, 1.70f, 3.10f, 4.60f, 2.20f};
  static const float rad[NB] = {30.0f, 26.0f, 34.0f, 22.0f, 28.0f};

  float bx[NB], by[NB], bk[NB];
  for (int i = 0; i < NB; i++) {
    bx[i] = CX + fsin(t * sx[i] + ph[i]) * 42.0f;
    by[i] = CY + fcos(t * sy[i] + ph[i] * 1.3f) * 42.0f;
    bk[i] = 1.0f / (rad[i] * rad[i]);
  }

  for (int y = y0; y < y1; y++) {
    float fy = (float)y;
    float dy2[NB];
    for (int i = 0; i < NB; i++) { float d = fy - by[i]; dy2[i] = d * d; }
    uint16_t* p = fb + y * SCR_W;
    for (int x = 0; x < SCR_W; x++) {
      float fx = (float)x;
      float f = 0.0f;
      for (int i = 0; i < NB; i++) {
        float dx = fx - bx[i];
        float k = 1.0f - (dx * dx + dy2[i]) * bk[i];
        if (k > 0.0f) f += k * k;
      }
      int idx = (int)(f * 200.0f);
      p[x] = pal[idx > 255 ? 255 : idx];
    }
  }
}

// ============================================================== flow
// Domain-warped noise -- sines whose inputs are themselves sines. This is
// the "pixels breathe" texture: the warp makes straight bands turn into
// something that looks like it is moving under its own power.

static void flowRender(uint16_t* fb, int y0, int y1, float t) {
  for (int y = y0; y < y1; y++) {
    float fy = (float)y * 0.045f;
    float qx = fsin(fy * 1.7f + t * 0.7f) * 1.4f;   // row-invariant warp
    uint16_t* p = fb + y * SCR_W;
    for (int x = 0; x < SCR_W; x++) {
      float fx = (float)x * 0.045f;
      float qy = fcos(fx * 1.3f - t * 0.5f) * 1.4f;
      float w  = fcos((fy + qy) * 1.9f + t) * 1.6f;
      float v  = fsin((fx + qx) * 2.1f + w + t * 0.6f)
               + 0.5f * fsin((fy + qy) * 3.3f - t * 0.9f);
      p[x] = pal[((int)((v + 1.5f) * 85.0f)) & 255];
    }
  }
}

// ============================================================== diag
// Panel bring-up aid: fills the whole frame with one flat colour, cycling
// red -> green -> blue -> white about every 1.5s. No gradients, no detail --
// if this does not visibly change, nothing is reaching the panel after the
// first frame, and the problem is the display path rather than the shading.

static void diagRender(uint16_t* fb, int y0, int y1, float t) {
  int phase = ((int)(t * 0.66f)) & 3;
  uint16_t c;
  switch (phase) {
    case 0:  c = rgb565(255, 0, 0);     break;
    case 1:  c = rgb565(0, 255, 0);     break;
    case 2:  c = rgb565(0, 0, 255);     break;
    default: c = rgb565(255, 255, 255); break;
  }
  for (int y = y0; y < y1; y++) {
    uint16_t* p = fb + y * SCR_W;
    for (int x = 0; x < SCR_W; x++) p[x] = c;
  }
}

// ============================================================== registry
const Shader shaders[] = {
  //  name          render         default palette
  { "plasma",    plasmaRender,  0 },   // cotton
  { "metaballs", metaRender,    1 },   // sorbet
  { "flow",      flowRender,    2 },   // seafoam
  { "diag",      diagRender,    5 },   // (writes colours directly)
};
const int shaderCount = sizeof(shaders) / sizeof(shaders[0]);
