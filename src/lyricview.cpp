#include "lyricview.h"
#include "gfx.h"
#include <string.h>
#include <stdio.h>

#define ROW_MAX   6
#define ROW_CHARS 48

enum Style {
  ST_PLAIN, ST_BIG, ST_SMALL, ST_BOXED, ST_INVERT,
  ST_TYPEWRITER, ST_SLIDE, ST_RISE, ST_WORD, ST_STAGGER, ST_COUNT
};

static const char* kStyleNames[ST_COUNT] = {
  "plain", "big", "small", "boxed", "invert",
  "typewriter", "slide", "rise", "word", "stagger"
};

// The palettes never go dark, so a dark glyph is what reads on them.
// ST_INVERT dims a band behind the text and flips this.
static uint16_t inkDark()  { return rgb565(38, 33, 48); }
static uint16_t inkLight() { return rgb565(250, 247, 252); }

static float easeOutCubic(float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  float u = 1.0f - t;
  return 1.0f - u * u * u;
}

// ---------------------------------------------------------------- helpers
static void dimRect(TFT_eSprite& s, int x, int y, int w, int h, int num, int den) {
  uint16_t* buf = (uint16_t*)s.getPointer();
  for (int yy = y; yy < y + h; yy++) {
    if (yy < 0 || yy >= SCR_H) continue;
    uint16_t* p = buf + yy * SCR_W;
    for (int xx = x; xx < x + w; xx++) {
      if (xx < 0 || xx >= SCR_W) continue;
      uint16_t c = p[xx];
      int r = ((c >> 11) & 0x1F) * num / den;
      int g = ((c >> 5)  & 0x3F) * num / den;
      int b = ( c        & 0x1F) * num / den;
      p[xx] = (uint16_t)((r << 11) | (g << 5) | b);
    }
  }
}

// Greedy word wrap measured against the font currently set on the sprite.
static int wrapText(TFT_eSprite& s, const char* txt,
                    char rows[][ROW_CHARS], int maxRows, int maxW) {
  int n = 0;
  const char* p = txt;
  char cur[ROW_CHARS];
  cur[0] = 0;

  while (*p && n < maxRows) {
    while (*p == ' ') p++;
    if (!*p) break;

    const char* ws = p;
    while (*p && *p != ' ') p++;
    int wlen = (int)(p - ws);
    if (wlen > ROW_CHARS - 1) wlen = ROW_CHARS - 1;
    char word[ROW_CHARS];
    memcpy(word, ws, wlen);
    word[wlen] = 0;

    char trial[ROW_CHARS * 2];
    if (cur[0]) snprintf(trial, sizeof(trial), "%s %s", cur, word);
    else        snprintf(trial, sizeof(trial), "%s", word);

    // A word wider than the whole line still gets its own row; it will clip,
    // which beats vanishing.
    if (s.textWidth(trial) <= maxW || cur[0] == 0) {
      strncpy(cur, trial, ROW_CHARS - 1);
      cur[ROW_CHARS - 1] = 0;
    } else {
      strncpy(rows[n], cur, ROW_CHARS - 1);
      rows[n][ROW_CHARS - 1] = 0;
      n++;
      strncpy(cur, word, ROW_CHARS - 1);
      cur[ROW_CHARS - 1] = 0;
    }
  }
  if (cur[0] && n < maxRows) {
    strncpy(rows[n], cur, ROW_CHARS - 1);
    rows[n][ROW_CHARS - 1] = 0;
    n++;
  }
  return n;
}

static int wordCount(const char* s) {
  int n = 0;
  bool in = false;
  for (; *s; s++) {
    if (*s == ' ') in = false;
    else if (!in) { in = true; n++; }
  }
  return n;
}

static Style styleFromSeed(uint32_t seed) {
  uint32_t h = seed * 2654435761u;
  h ^= h >> 15;
  return (Style)(h % ST_COUNT);
}

const char* lyricStyleName(uint32_t seed) {
  return kStyleNames[styleFromSeed(seed)];
}

// Deterministic pick, with fallbacks so a style never overflows the panel.
static Style chooseStyle(const char* text, uint32_t seed) {
  Style st = styleFromSeed(seed);
  int len = (int)strlen(text);

  // Font 4 is 26px tall and ~14px wide: long text simply will not fit.
  if (st == ST_BIG && len > 22) st = ST_PLAIN;
  if (st == ST_WORD && wordCount(text) < 2) st = ST_BIG;
  if (st == ST_WORD && len > 60) st = ST_SMALL;
  // Very long lines need the small font to stay on screen at all.
  if (len > 64 && st != ST_SMALL) st = ST_SMALL;
  return st;
}

// Copies the nth space-delimited word of `text` into `out`.
static void nthWord(const char* text, int idx, char* out, int outSize) {
  const char* p = text;
  for (int i = 0; i < idx; i++) {
    while (*p == ' ') p++;
    while (*p && *p != ' ') p++;
  }
  while (*p == ' ') p++;
  int n = 0;
  while (p[n] && p[n] != ' ' && n < outSize - 1) { out[n] = p[n]; n++; }
  out[n] = 0;
}

// ---------------------------------------------------------------- main draw
void lyricDraw(TFT_eSprite& s, const char* text,
               uint32_t ageMs, uint32_t holdMs, uint32_t seed) {
  if (!text || !*text) return;

  Style st = chooseStyle(text, seed);

  int  font    = 2;
  int  maxRows = 4;
  bool light   = false;

  switch (st) {
    case ST_BIG:
    case ST_WORD:   font = 4; maxRows = 3; break;
    case ST_SMALL:  font = 1; maxRows = 6; break;
    case ST_INVERT: font = 2; maxRows = 4; light = true; break;
    default:        font = 2; maxRows = 4; break;
  }

  s.setTextFont(font);
  s.setTextDatum(TL_DATUM);

  // ST_WORD shows one word at a time, paced across the line's time on screen.
  char single[ROW_CHARS];
  const char* src = text;
  if (st == ST_WORD) {
    int wc = wordCount(text);
    uint32_t span = holdMs ? holdMs : 2000;
    int idx = (int)((uint64_t)ageMs * (uint64_t)wc / (uint64_t)span);
    if (idx >= wc) idx = wc - 1;
    if (idx < 0) idx = 0;
    nthWord(text, idx, single, sizeof(single));
    src = single;
  }

  char rows[ROW_MAX][ROW_CHARS];
  int margin = (st == ST_BOXED || st == ST_INVERT) ? 8 : 4;
  int nRows  = wrapText(s, src, rows, maxRows, SCR_W - margin * 2);
  if (nRows == 0) return;

  int lineH  = s.fontHeight() + 1;
  int blockH = nRows * lineH;
  int baseY  = (SCR_H - blockH) / 2;

  // Entry animation is motion and reveal rather than alpha: the sprite has no
  // blending, and the shader behind it is different every frame anyway.
  float in = easeOutCubic((float)ageMs / 320.0f);
  int dx = 0, dy = 0;
  int reveal = -1;                     // -1 draws the whole string

  if (st == ST_SLIDE) {
    dx = (int)((1.0f - in) * ((seed & 1) ? 90.0f : -90.0f));
  } else if (st == ST_RISE) {
    dy = (int)((1.0f - in) * 34.0f);
  } else if (st == ST_TYPEWRITER) {
    uint32_t span = holdMs ? (holdMs * 55 / 100) : 900;
    if (span < 120) span = 120;
    int total = (int)strlen(src);
    reveal = (int)((uint64_t)ageMs * (uint64_t)total / (uint64_t)span);
    if (reveal > total) reveal = total;
  }
  baseY += dy;

  if (st == ST_BOXED) {
    int pad = 4;
    s.fillRect(margin - pad, baseY - pad,
               SCR_W - (margin - pad) * 2, blockH + pad * 2,
               rgb565(252, 249, 253));
  } else if (st == ST_INVERT) {
    int pad = 5;
    dimRect(s, 0, baseY - pad, SCR_W, blockH + pad * 2, 32, 100);
  }

  s.setTextColor(light ? inkLight() : inkDark());

  int shown = 0;
  for (int i = 0; i < nRows; i++) {
    char* row = rows[i];

    char clipped[ROW_CHARS];
    if (reveal >= 0) {
      int rowLen = (int)strlen(row);
      int take = reveal - shown;
      if (take <= 0) break;
      if (take > rowLen) take = rowLen;
      memcpy(clipped, row, take);
      clipped[take] = 0;
      shown += rowLen + 1;             // +1 for the space the wrap consumed
      row = clipped;
    }

    int x = (SCR_W - s.textWidth(row)) / 2 + dx;
    if (st == ST_STAGGER) x += (i & 1) ? 12 : -12;
    if (st == ST_SMALL)   x = margin;  // small font reads better left-aligned

    s.drawString(row, x, baseY + i * lineH);
  }
}

// ---------------------------------------------------------------- fallback
void lyricDrawNowPlaying(TFT_eSprite& s, const char* track, const char* artist) {
  char rows[ROW_MAX][ROW_CHARS];

  s.setTextDatum(TL_DATUM);
  s.setTextColor(inkDark());

  s.setTextFont(2);
  int nT = wrapText(s, track, rows, 3, SCR_W - 8);
  int lineH = s.fontHeight() + 1;
  int y = SCR_H / 2 - (nT * lineH) / 2 - 12;
  for (int i = 0; i < nT; i++) {
    int x = (SCR_W - s.textWidth(rows[i])) / 2;
    s.drawString(rows[i], x, y + i * lineH);
  }

  s.setTextFont(1);
  char aRows[ROW_MAX][ROW_CHARS];
  int nA = wrapText(s, artist, aRows, 2, SCR_W - 8);
  int ay = y + nT * lineH + 6;
  int aH = s.fontHeight() + 1;
  for (int i = 0; i < nA; i++) {
    int x = (SCR_W - s.textWidth(aRows[i])) / 2;
    s.drawString(aRows[i], x, ay + i * aH);
  }
}
