// LilyGo T-QT Pro -- shader engine
//
// Frame path:
//   core 0 worker renders rows [0,64)      \  into the same back buffer,
//   core 1 loop   renders rows [64,128)    /  no overlap, no locking
//   loop pushes the finished buffer to the panel and flips
//
// Both 32KB buffers live in internal SRAM. At 128x128 that is affordable,
// which is why this board suits per-pixel work better than physically larger
// panels that have to stage frames through PSRAM.
//
// The present path deliberately uses only TFT_eSPI's ordinary blocking calls,
// each frame wrapped in its own startWrite/endWrite. Driving the panel through
// initDMA() instead hands SPI3 to the ESP-IDF driver while TFT_eSPI still
// writes the same peripheral's registers directly; the first frame lands and
// nothing after it does.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>

#include "gfx.h"
#include "shaders.h"

#define PIN_BTN_L  0    // next effect
#define PIN_BTN_R  47   // toggle the FPS readout
#define PIN_LCD_BL 10   // active low

static TFT_eSPI tft;

static uint16_t*          fb[2] = {nullptr, nullptr};
static int                cur   = 0;
static uint16_t* volatile gFb   = nullptr;
static volatile float     gT    = 0.0f;
static volatile int       gShader = 0;

static SemaphoreHandle_t startSem, doneSem;

static int  fps      = 0;
static bool showFps  = true;
static int  gPalette = 0;

// ------------------------------------------------------------------ worker
static void workerLoop(void*) {
  for (;;) {
    xSemaphoreTake(startSem, portMAX_DELAY);
    shaders[gShader].render(gFb, 0, SCR_H / 2, gT);
    xSemaphoreGive(doneSem);
  }
}

// ------------------------------------------------------------------ buttons
struct Btn { uint8_t pin; bool last; uint32_t tEdge; };
static Btn btnL = {PIN_BTN_L, true, 0};
static Btn btnR = {PIN_BTN_R, true, 0};

// Active low with an internal pullup, matching LilyGo's own examples.
static bool clicked(Btn& b) {
  bool now = digitalRead(b.pin);
  uint32_t ms = millis();
  if (now != b.last && (ms - b.tEdge) > 30) {
    b.tEdge = ms;
    b.last  = now;
    if (!now) return true;            // falling edge = press
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
  selectPalette(shaders[gShader].defaultPal);   // sensible default, cycle freely
  Serial.printf("[shader] %d/%d  %s\n", gShader + 1, shaderCount,
                shaders[gShader].name);
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

  const size_t bytes = SCR_PX * sizeof(uint16_t);
  for (int i = 0; i < 2; i++) {
    fb[i] = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!fb[i]) {
      Serial.printf("FATAL: framebuffer %d alloc failed (%u bytes)\n", i, (unsigned)bytes);
      while (true) delay(1000);
    }
    memset(fb[i], 0, bytes);
    Serial.printf("fb[%d] @ %p  %u bytes\n", i, fb[i], (unsigned)bytes);
  }
  Serial.printf("free internal heap: %u bytes\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  tft.init();
  tft.setRotation(0);
  // Our buffers hold pixels in the panel's wire byte order; this swaps them
  // back into TFT_eSPI's convention so pushPixels emits them correctly.
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);       // TFT_BACKLIGHT_ON == 0

  startSem = xSemaphoreCreateBinary();
  doneSem  = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(workerLoop, "render0", 4096, nullptr, 2, nullptr, 0);

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
  static uint32_t renderAcc = 0;      // us spent shading over the window

  if (clicked(btnL)) selectShader(gShader + 1);
  if (clicked(btnR)) selectPalette(gPalette + 1);

  while (Serial.available()) {
    int c = Serial.read();
    if      (c == 'n') selectShader(gShader + 1);
    else if (c == 'p') selectShader(gShader - 1);
    else if (c == 'f') showFps = !showFps;
    else if (c == 'c') selectPalette(gPalette + 1);
  }

  uint32_t now = micros();
  float dt = (float)(now - lastUs) * 1e-6f;
  lastUs = now;
  // Keep t small enough that float still resolves below a millisecond.
  // Wraps about every 2.8h with a single frame of discontinuity.
  tAcc += dt;
  if (tAcc > 10000.0f) tAcc -= 10000.0f;

  gFb = fb[cur];
  gT  = tAcc;

  uint32_t rStart = micros();
  xSemaphoreGive(startSem);                                  // core 0: top half
  shaders[gShader].render(fb[cur], SCR_H / 2, SCR_H, tAcc);  // core 1: bottom
  xSemaphoreTake(doneSem, portMAX_DELAY);
  renderAcc += micros() - rStart;

  if (showFps) drawNum(fb[cur], 3, 3, fps, rgb565(70, 62, 82), 2);

  tft.startWrite();
  tft.setAddrWindow(0, 0, SCR_W, SCR_H);
  tft.pushPixels(fb[cur], SCR_PX);
  tft.endWrite();

  cur ^= 1;

  frames++;
  uint32_t ms = millis();
  if (ms - fpsMark >= 1000) {
    fps = frames;
    float renderMs = frames ? (float)renderAcc / (float)frames / 1000.0f : 0.0f;
    Serial.printf("%-10s %3d fps  render %.2f ms  t=%.1fs\n",
                  shaders[gShader].name, fps, renderMs, tAcc);
    frames    = 0;
    renderAcc = 0;
    fpsMark   = ms;
  }
}
