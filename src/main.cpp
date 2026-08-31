// LilyGo T-QT Pro -- shader engine with synced Spotify lyrics
//
// Frame path:
//   core 0 worker renders rows [0,64)      \  into the same back sprite,
//   core 1 loop   renders rows [64,128)    /  no overlap, no locking
//   loop draws the current lyric line on top, then pushes the sprite
//
// Both 32KB sprites are forced into internal SRAM (setAttribute PSRAM_ENABLE
// false); TFT_eSPI defaults to PSRAM when it is available, which would put a
// slow bus in the middle of every frame.
//
// The present path uses ordinary blocking startWrite/pushSprite/endWrite.
// Driving the panel through initDMA() instead hands SPI3 to the ESP-IDF driver
// while TFT_eSPI still writes the same peripheral's registers directly; the
// first frame lands and nothing after it does.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>

#include "gfx.h"
#include "shaders.h"
#include "lyrics.h"
#include "lyricview.h"
#include "net.h"

#define PIN_BTN_L  0    // next effect
#define PIN_BTN_R  47   // next palette
#define PIN_LCD_BL 10   // active low

#define SPOTIFY_POLL_MS 5000

static TFT_eSPI   tft;
static TFT_eSprite spr[2] = {TFT_eSprite(&tft), TFT_eSprite(&tft)};

static uint16_t*          fb[2] = {nullptr, nullptr};
static int                cur   = 0;
static uint16_t* volatile gFb   = nullptr;
static volatile float     gT    = 0.0f;
static volatile int       gShader = 0;

static SemaphoreHandle_t startSem, doneSem;

static int  fps      = 0;
static bool showFps  = false;
static int  gPalette = 0;

// ------------------------------------------------------------------ playback
static Lyrics     lyrics;
static NowPlaying np;
static char       loadedTrack[100] = {0};   // which track lyrics were fetched for
static bool       haveLyrics = false;
static int        lastLineIdx = -2;

// Progress is interpolated between polls so lyrics stay in time without
// hammering the API.
static volatile uint32_t progressBaseMs = 0;   // progress at last poll
static volatile uint32_t progressAtMs   = 0;   // millis() of that poll
static volatile bool     isPlaying      = false;

static uint32_t playbackMs() {
  if (!isPlaying) return progressBaseMs;
  return progressBaseMs + (millis() - progressAtMs);
}

// ------------------------------------------------------------------ demo
// Placeholder text, not lyrics: deliberately varied lengths so every style
// and the word-wrap get exercised when no credentials are configured.
static const char* kDemoLrc =
    "[00:00.00]shader engine online\n"
    "[00:02.50]waiting for spotify\n"
    "[00:05.00]add src/secrets.h\n"
    "[00:07.50]then it follows whatever you play\n"
    "[00:11.00]one line at a time\n"
    "[00:13.50]in a different style each time\n"
    "[00:16.50]typed, boxed, staggered, one word\n"
    "[00:20.00]\n"
    "[00:22.00]press the left button for effects\n"
    "[00:25.00]the right one for palettes\n"
    "[00:28.00]\n";

// ------------------------------------------------------------------ worker
static void workerLoop(void*) {
  for (;;) {
    xSemaphoreTake(startSem, portMAX_DELAY);
    shaders[gShader].render(gFb, 0, SCR_H / 2, gT);
    xSemaphoreGive(doneSem);
  }
}

// ------------------------------------------------------------------ network
static void netTask(void*) {
  netBegin();
  for (;;) {
    if (netConnected()) {
      NowPlaying fresh;
      memset(&fresh, 0, sizeof(fresh));
      if (netPollSpotify(fresh)) {
        if (fresh.valid) {
          progressBaseMs = fresh.progressMs;
          progressAtMs   = millis();
          isPlaying      = fresh.playing;

          if (strcmp(fresh.track, loadedTrack) != 0) {
            Serial.printf("[track] %s -- %s\n", fresh.track, fresh.artist);
            String body;
            if (netFetchLyrics(fresh, body)) {
              int n = lyrics.parse(body.c_str());
              haveLyrics = n > 0;
              Serial.printf("[lyrics] %d synced lines\n", n);
            } else {
              lyrics.clear();
              haveLyrics = false;
              Serial.println("[lyrics] none on lrclib for this track");
            }
            strncpy(loadedTrack, fresh.track, sizeof(loadedTrack) - 1);
            loadedTrack[sizeof(loadedTrack) - 1] = 0;
            lastLineIdx = -2;
          }
          np = fresh;
        } else {
          isPlaying = false;
          np.valid = false;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(SPOTIFY_POLL_MS));
  }
}

// ------------------------------------------------------------------ buttons
struct Btn { uint8_t pin; bool last; uint32_t tEdge; };
static Btn btnL = {PIN_BTN_L, true, 0};
static Btn btnR = {PIN_BTN_R, true, 0};

static bool clicked(Btn& b) {
  bool now = digitalRead(b.pin);
  uint32_t ms = millis();
  if (now != b.last && (ms - b.tEdge) > 30) {
    b.tEdge = ms;
    b.last  = now;
    if (!now) return true;
  }
  return false;
}

static void selectPalette(int i) {
  gPalette = ((i % paletteCount) + paletteCount) % paletteCount;
  applyPalette(gPalette);
  Serial.printf("[palette] %d/%d  %s\n", gPalette + 1, paletteCount,
                palettes[gPalette].name);
}

static void selectShader(int i) {
  gShader = ((i % shaderCount) + shaderCount) % shaderCount;
  selectPalette(shaders[gShader].defaultPal);
  Serial.printf("[shader] %d/%d  %s\n", gShader + 1, shaderCount,
                shaders[gShader].name);
}

// ------------------------------------------------------------------ selftest
// Runs on the target rather than the host: same compiler, same int widths.
static void selfTestLyrics() {
  static const char* sample =
      "[ar:Placeholder Artist]\n"
      "[length:03:00]\n"
      "[00:01.50]alpha\n"
      "[00:10.25]bravo\n"
      "[01:05.00]charlie\n"
      "[00:30.00][02:00.00]delta\n"
      "[00:45.00]\n";

  // Heap, not stack: a Lyrics is ~14KB and loopTask only gets 8KB.
  Lyrics* tp = new Lyrics();
  if (!tp) { Serial.println("[selftest] alloc failed"); return; }
  Lyrics& t = *tp;

  int n = t.parse(sample);
  int pass = 0, total = 0;

  auto check = [&](const char* what, bool ok) {
    total++;
    if (ok) pass++;
    else Serial.printf("  FAIL %s\n", what);
  };

  check("metadata skipped, 6 entries", n == 6);
  check("first stamp 1500ms",     t.timeAt(0) == 1500);
  check("centisecond scaling",    t.timeAt(1) == 10250);
  check("multi-stamp sorted",     t.timeAt(2) == 30000);
  check("empty body kept",        t.timeAt(3) == 45000);
  check("minutes carry",          t.timeAt(4) == 65000);
  check("second stamp of pair",   t.timeAt(5) == 120000);
  check("text parsed",            strcmp(t.text(0), "alpha") == 0);
  check("repeat text duplicated", strcmp(t.text(5), "delta") == 0);
  check("gap line empty",         t.text(3)[0] == 0);
  check("before first is -1",     t.indexAt(0) == -1);
  check("exact boundary hits",    t.indexAt(1500) == 0);
  check("between stamps holds",   t.indexAt(9000) == 0);
  check("past last stays last",   t.indexAt(999999) == 5);

  Serial.printf("[selftest] lyrics %d/%d passed\n", pass, total);
  delete tp;
}

// ------------------------------------------------------------------ setup
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) delay(10);

  Serial.println();
  Serial.println("=== T-QT Pro shader engine ===");

  pinMode(PIN_BTN_L, INPUT_PULLUP);
  pinMode(PIN_BTN_R, INPUT_PULLUP);

  gfxInit();
  selfTestLyrics();

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);          // TFT_BACKLIGHT_ON == 0

  // This SDK sets CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096, so anything over
  // 4KB goes to PSRAM -- including a 32KB sprite. TFT_eSPI's PSRAM_ENABLE
  // attribute does not help: it only picks ps_calloc vs calloc, and plain
  // calloc lands externally too. Raise the threshold across createSprite so
  // the framebuffers come from internal SRAM, then put it back so the TLS
  // stack can still use PSRAM and leave internal RAM for us.
  heap_caps_malloc_extmem_enable(1 << 20);
  for (int i = 0; i < 2; i++) {
    spr[i].setColorDepth(16);
    fb[i] = (uint16_t*)spr[i].createSprite(SCR_W, SCR_H);
    if (!fb[i]) {
      Serial.printf("FATAL: sprite %d alloc failed\n", i);
      while (true) delay(1000);
    }
    Serial.printf("sprite[%d] @ %p  %s\n", i, fb[i],
                  esp_ptr_external_ram(fb[i]) ? "PSRAM (slow!)" : "internal SRAM");
  }
  heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
  Serial.printf("free internal heap: %u bytes\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  startSem = xSemaphoreCreateBinary();
  doneSem  = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(workerLoop, "render0", 4096, nullptr, 2, nullptr, 0);

  if (netEnabled()) {
    // 16KB: an mbedTLS handshake with bundle verification is stack-hungry.
    xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
    Serial.println("[net] credentials present, polling Spotify");
  } else {
    Serial.println("[net] no src/secrets.h -- demo mode");
  }

  // Demo lyrics show until a real track arrives.
  lyrics.parse(kDemoLrc);
  haveLyrics = true;

  selectShader(0);
  Serial.println("BTN_L (GPIO0) next effect   BTN_R (GPIO47) next palette");
  Serial.println("serial: n/p effect, c palette, f readout");
}

// ------------------------------------------------------------------ loop
void loop() {
  static uint32_t lastUs    = micros();
  static float    tAcc      = 0.0f;
  static uint32_t fpsMark   = millis();
  static int      frames    = 0;
  static uint32_t renderAcc = 0;
  static uint32_t demoStart = millis();

  if (clicked(btnL)) selectShader(gShader + 1);
  if (clicked(btnR)) selectPalette(gPalette + 1);

  while (Serial.available()) {
    int c = Serial.read();
    if      (c == 'n') selectShader(gShader + 1);
    else if (c == 'p') selectShader(gShader - 1);
    else if (c == 'c') selectPalette(gPalette + 1);
    else if (c == 'f') showFps = !showFps;
  }

  uint32_t now = micros();
  float dt = (float)(now - lastUs) * 1e-6f;
  lastUs = now;
  tAcc += dt;
  if (tAcc > 10000.0f) tAcc -= 10000.0f;

  gFb = fb[cur];
  gT  = tAcc;

  uint32_t rStart = micros();
  xSemaphoreGive(startSem);
  shaders[gShader].render(fb[cur], SCR_H / 2, SCR_H, tAcc);
  xSemaphoreTake(doneSem, portMAX_DELAY);
  renderAcc += micros() - rStart;

  // ---- lyric overlay
  bool live = netEnabled() && np.valid;
  uint32_t posMs = live ? playbackMs()
                        : ((millis() - demoStart) % 31000u);

  if (haveLyrics && !lyrics.empty()) {
    int idx = lyrics.indexAt(posMs);
    if (idx != lastLineIdx) {
      if (idx >= 0 && lyrics.text(idx)[0])
        Serial.printf("[line %d] %s\n", idx, lyricStyleName((uint32_t)idx));
      lastLineIdx = idx;
    }
    if (idx >= 0) {
      uint32_t startMs = lyrics.timeAt(idx);
      uint32_t nextMs  = (idx + 1 < lyrics.count()) ? lyrics.timeAt(idx + 1) : startMs + 4000;
      uint32_t age     = posMs > startMs ? posMs - startMs : 0;
      uint32_t hold    = nextMs > startMs ? nextMs - startMs : 3000;
      lyricDraw(spr[cur], lyrics.text(idx), age, hold, (uint32_t)idx);
    }
  } else if (live) {
    lyricDrawNowPlaying(spr[cur], np.track, np.artist);
  }

  if (showFps) {
    spr[cur].setTextFont(1);
    spr[cur].setTextDatum(TL_DATUM);
    spr[cur].setTextColor(rgb565(70, 62, 82));
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", fps);
    spr[cur].drawString(buf, 3, 3);
  }

  tft.startWrite();
  spr[cur].pushSprite(0, 0);
  tft.endWrite();

  cur ^= 1;

  frames++;
  uint32_t ms = millis();
  if (ms - fpsMark >= 1000) {
    fps = frames;
    float renderMs = frames ? (float)renderAcc / (float)frames / 1000.0f : 0.0f;
    Serial.printf("%-10s %3d fps  render %.2f ms  %s\n",
                  shaders[gShader].name, fps, renderMs, netStatus());
    frames    = 0;
    renderAcc = 0;
    fpsMark   = ms;
  }
}
