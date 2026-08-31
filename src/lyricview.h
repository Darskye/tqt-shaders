#pragma once
#include <TFT_eSPI.h>
#include <stdint.h>

// Draws one lyric line over whatever the shader already put in the sprite.
//
// The style is chosen deterministically from `seed` (the line index), so it
// stays put for that line's whole time on screen instead of flickering between
// frames, but changes from line to line. Styles that cannot fit the text fall
// back rather than overflow.
//
//   ageMs  -- how long this line has been showing
//   holdMs -- how long until the next line takes over (0 if unknown)
void lyricDraw(TFT_eSprite& s, const char* text,
               uint32_t ageMs, uint32_t holdMs, uint32_t seed);

// Fallback for tracks with no synced lyrics.
void lyricDrawNowPlaying(TFT_eSprite& s, const char* track, const char* artist);

const char* lyricStyleName(uint32_t seed);
