# T-QT Pro shader engine

Per-pixel generative graphics on a LilyGo T-QT Pro (ESP32-S3FN4R2, 0.85" 128x128),
with time-synced Spotify lyrics drawn over the top in a different style each line.

Measured on hardware:

| effect | fps | render | push |
|---|---|---|---|
| plasma | 108 | 1.82 ms | 7.44 ms |
| flow | 95 | 3.07 ms | 7.46 ms |
| metaballs | 81 | 4.93 ms | 7.42 ms |
| diag (flat fill) | 133 | 0.06 ms | 7.46 ms |

Frame time is `render + 7.44 ms`. The push cost is flat across every effect
because it is the SPI transfer, not the shading: 128x128 at 16bpp is 32 KB, and
at 40 MHz that is ~6.55 ms on the wire plus address-window and loop overhead.

## Lyrics

Two sources, because Spotify has no lyrics API -- their in-app lyrics are
licensed from Musixmatch and are not exposed:

- **Spotify Web API** (`/v1/me/player/currently-playing`) for the track, artist,
  album and playback position. Polled every 5s; position is interpolated locally
  in between so the timing stays tight without hammering the API.
- **[LRCLIB](https://lrclib.net)** for synced LRC lyrics. Free, no API key,
  no account. Fetched once per track change.

Unofficial endpoints that scrape Spotify's internal lyrics service with a
session cookie are deliberately not used: they break their terms, and they break.

TLS is validated against the Arduino core's root CA bundle
(`setCACertBundle(rootca_crt_bundle_start)`), not `setInsecure()` -- a refresh
token crosses that connection.

### Styles

At ~16 characters per row there is no room for a lyric sheet, so it shows one
line at a time and varies the presentation instead. The style is picked
deterministically from the line index, so it holds steady for that line and
changes on the next one. Styles that cannot fit fall back rather than overflow.

| style | what it does |
|---|---|
| plain | centred, 16px |
| big | 26px font, short lines only |
| small | 8px font, left aligned, for long lines |
| boxed | text on a near-white card |
| invert | background band dimmed, light text |
| typewriter | reveals across the line's own duration |
| slide | enters horizontally, eased |
| rise | enters from below, eased |
| word | one word at a time, paced across the line |
| stagger | wrapped rows offset alternately |

Entry animations are motion and reveal rather than fades: the sprite has no
alpha blending, and the shader behind it changes every frame anyway.

Without `src/secrets.h` the firmware still builds and runs, showing a demo
sequence so the styles are visible.

## Why it's quick

At 128x128 a frame is 32 KB, so **both buffers fit in internal SRAM** — no PSRAM
staging, which is what throttles per-pixel work on physically larger panels.

Rendering splits across both cores: core 0 does rows 0-63, core 1 does 64-127,
into the same back sprite. The ranges don't overlap so no locking is needed.

**The sprites must be forced into internal SRAM.** This SDK sets
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, so a 32KB sprite goes to PSRAM by
default -- costing ~0.4ms per frame of render time alone. TFT_eSPI's
`setAttribute(PSRAM_ENABLE, false)` does *not* fix this: it only chooses between
`ps_calloc` and `calloc`, and plain `calloc` lands externally too. The fix is to
raise the threshold with `heap_caps_malloc_extmem_enable()` across
`createSprite()` and restore it afterwards, so the TLS stack can still use PSRAM.

Budget for new effects: to hold 60 fps you have `16.7 - 7.4 = 9.3 ms` of render
time. Metaballs, the heaviest effect here, uses 4.93 ms — so there is roughly 2x
headroom before you drop under 60.

## Why it does not use DMA

An earlier version drove the panel with `tft.initDMA()` + `pushImageDMA()` and
reported a genuine 152 fps, with `t` advancing and the framebuffer checksum
changing every frame — but **only the first frame ever appeared on the panel.**

`initDMA()` calls `spi_bus_initialize()`, handing SPI3 to the ESP-IDF SPI driver,
while TFT_eSPI's ordinary blocking path keeps writing that same peripheral's
registers directly. Two drivers own the bus. Once a DMA transaction has run, the
peripheral is left in a state where subsequent register-driven writes stop
reaching the display — which is why switching to a blocking push *while
`initDMA()` was still in effect* did not help either, and made this look like a
shader bug rather than a bus-ownership bug.

The fix was to drop DMA entirely and wrap each frame in its own
`startWrite()` / `endWrite()`. Costs ~7.4 ms of CPU per frame that DMA would
overlap with rendering. Reclaiming it means driving the panel from a private
`spi_device_handle_t` instead of TFT_eSPI's, which would put the ceiling back
near 150 fps. Not attempted; 81-108 fps is already past the point of visible
smoothness.

If you do revisit it: holding a single `startWrite()` open for the whole session
is also wrong, because it never lets TFT_eSPI toggle CS between frames.

## Hardware

| | |
|---|---|
| MCU | ESP32-S3FN4R2, 4 MB flash + 2 MB PSRAM, 240 MHz |
| Panel | 128x128, GC9A01 driver with CGRAM offset (colstart 2, rowstart 1) |
| SPI | HSPI @ 40 MHz — MOSI 2, SCLK 3, CS 5, DC 6, RST 1 |
| Backlight | GPIO 10, **active low** |
| Buttons | GPIO 0 (left), GPIO 47 (right) — active low, internal pullup |
| Battery sense | GPIO 4 |

`platformio.ini` sets `board_build.arduino.memory_type = qio_qspi`. LilyGo's own
board file declares "No PSRAM" because it targets the 8 MB variant; on an FN4R2
that silently discards 2 MB. Nothing here needs PSRAM — the framebuffers are
deliberately in internal SRAM — but the config should match the silicon.

## Build

    python -m platformio run -t upload
    python -m platformio device monitor

## Connecting Spotify

1. Create an app at https://developer.spotify.com/dashboard and add
   `http://127.0.0.1:8888/callback` as a Redirect URI.
2. `python tools/spotify_auth.py` -- authorises in your browser and prints a
   refresh token. It runs entirely on your machine; nothing is sent anywhere
   but Spotify, and the token is not written to disk.
3. `cp src/secrets.h.example src/secrets.h`, fill in WiFi plus the three
   Spotify values, reflash.

`src/secrets.h` is gitignored and must stay that way -- this repo is public.
Scopes requested are read-only (`user-read-currently-playing`,
`user-read-playback-state`).

## Controls

| input | action |
|---|---|
| GPIO 0 button | next effect |
| GPIO 47 button | next palette |
| serial `n` / `p` | next / previous effect |
| serial `c` | next palette |
| serial `f` | toggle FPS readout |

## Palettes

Six lo-fi pastels, cycled independently of the effect. Each effect names a
sensible default in its registry row; changing effect applies that default and
you can cycle away from it freely.

| name | reads as | peak saturation |
|---|---|---|
| cotton | blush -> lavender -> periwinkle | 0.38 |
| sorbet | butter -> peach -> lilac | 0.40 |
| seafoam | mint -> sage -> rose | 0.37 |
| dusk | rose -> violet -> cornflower | 0.40 |
| matcha | pistachio -> sage -> lilac | 0.40 |
| faded | sand -> mauve -> dusty blue | 0.25 |

The pastel quality comes from the `a` and `b` terms, not the hue phases. A high
baseline (`a` around 0.75) with a small amplitude (`b` around 0.15) keeps every
channel between roughly 0.5 and 0.97, so nothing bottoms out and nothing pins.
The stock saturated rainbow is `a = 0.5, b = 0.5` — that full swing is what
makes the usual cosine palette look like neon.

Verified across the ramp: min channel 0.48, max 0.97, no clipping.

Because the field is light everywhere, the FPS readout draws in charcoal
(`rgb565(70, 62, 82)`) rather than white, which would disappear.

## Writing a new effect

Add a function and a table row in `src/shaders.cpp`. That's the whole API:

```cpp
static void myPalette() {
  // Inigo Quilez cosine palette: a + b*cos(2pi*(c*t + d))
  buildPalette({0.5f,0.5f,0.5f}, {0.5f,0.5f,0.5f},
               {1.0f,1.0f,1.0f}, {0.0f,0.33f,0.67f});
}

static void myRender(uint16_t* fb, int y0, int y1, float t) {
  for (int y = y0; y < y1; y++) {           // only your row range
    uint16_t* p = fb + y * SCR_W;
    for (int x = 0; x < SCR_W; x++) {
      float v = fsin(x * 0.05f + t) + fsin(y * 0.05f - t);
      p[x] = pal[((int)((v + 2.0f) * 63.7f)) & 255];
    }
  }
}
```

then register it:

```cpp
{ "myeffect", myRender, myPalette },
```

Rules that keep it fast:

- **Render only `[y0, y1)`.** The other core owns the rest of the frame.
- **Use `fsin` / `fcos`, not `sinf` / `cosf`.** Table lookup, wraps for free,
  handles negative angles.
- **Compute a scalar, index `pal[]`.** One lookup per pixel beats colour maths
  per pixel. The cosine palette is cyclic, so `& 255` wraps seamlessly rather
  than banding.
- **Hoist anything that only depends on `y`** out of the inner loop.
- **`radTab[y*SCR_W + x]`** is the precomputed distance from centre, so you never
  need a per-pixel `sqrtf`.
- **Avoid division.** Metaballs uses a clamped quartic falloff rather than the
  textbook `r^2/d^2` for exactly this reason.
- Write colours with `rgb565()`, which emits the panel's wire byte order.
  `setSwapBytes(true)` in `setup()` swaps them back for `pushPixels`.

## Panel revisions

These boards ship with two panels needing different init sequences. The vendored
TFT_eSPI is set up for the **new** panel, which is what this board turned out to
be — confirmed by a flat-fill test rendering correct red/green/blue. If you ever
see inverted colours or a couple of pixels of offset on another unit:

    cp panels/GC9A01_Init.old_panel.h     lib/TFT_eSPI/TFT_Drivers/GC9A01_Init.h
    cp panels/GC9A01_Rotation.old_panel.h lib/TFT_eSPI/TFT_Drivers/GC9A01_Rotation.h

## Known noise

`spiAttachMISO(): HSPI Does not have default pins on ESP32S3!` prints once at
boot. TFT_eSPI initialising with `TFT_MISO = -1`, correct for this board — the
panel is write-only. Harmless.

`t` wraps at 10000 s (~2.8 h) to keep float time resolution below a millisecond;
expect one frame of discontinuity when it does.

## Credits

`lib/TFT_eSPI/` is vendored verbatim from
[LilyGo's T-QT repo](https://github.com/Xinyuan-LilyGO/T-QT) (TFT_eSPI 2.5.43,
by Bodmer, originally derived from Adafruit_ILI9341 — see `lib/TFT_eSPI/license.txt`).
It is included rather than pulled as a dependency because LilyGo patch the
GC9A01 init sequence and ship `Setup211_LilyGo_T_QT_Pro_S3.h`; stock upstream
TFT_eSPI will not bring this panel up correctly.

`panels/` holds both panel-revision init sequences from that same repo so either
board variant can be configured without re-cloning it.

Everything in `src/` is original.
