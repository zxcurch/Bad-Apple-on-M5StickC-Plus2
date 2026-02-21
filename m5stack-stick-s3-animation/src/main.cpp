#include <assert.h>
#include <M5Unified.h>
#include <LittleFS.h>
#include <stdlib.h>

// ---- File paths ----
static const char *VIDEO_FILE = "/bad_apple.bin";

// ---- Video file header (12 bytes, packed) ----
#pragma pack(push, 1)
struct FileHeader {
  uint16_t width;
  uint16_t height;
  uint32_t total_frames;
  uint16_t fps;
  uint16_t flags;
};
#pragma pack(pop)

// ---- Display ----
static const uint16_t DISP_W = 240;
static const uint16_t DISP_H = 135;

// ---- Sprites for flicker-free rendering ----
static M5Canvas canvas;       // full-screen buffer (240x135)
static M5Canvas videoSprite;  // video frame buffer (vidW x vidH)

// ---- Color state ----
static volatile uint16_t fgColor = 0xFFFF;
static volatile uint16_t bgColor = 0x0000;
static volatile bool invertColors = false;

// ---- Pause state ----
static volatile bool paused = false;

// ---- Glitch (unused without IMU, but kept for compatibility) ----
static volatile int glitchFrames = 0;

// ---- Smooth rotation ----
static float smoothAngle = 0.0f;
static const float ANGLE_SMOOTHING = 0.25f;

// ---- Video state ----
static uint16_t vidW, vidH;
static uint32_t totalFrames;
static uint16_t vidFps;
static uint32_t *frameIndex = nullptr;
static size_t frameDataStart;

// ---- Buffers (now in normal RAM) ----
static uint8_t *rleBuf = nullptr;
static uint16_t *rgb565Buf = nullptr;
static const size_t MAX_RLE_SIZE = 16384;

// ---- HSV to RGB565 ----
uint16_t hsvToRgb565(uint16_t h, uint8_t s, uint8_t v) {
  uint8_t region = h / 60;
  uint8_t remainder = (h - region * 60) * 255 / 60;
  uint8_t p = ((uint16_t)v * (255 - s)) >> 8;
  uint8_t q = ((uint16_t)v * (255 - ((uint16_t)s * remainder >> 8))) >> 8;
  uint8_t t = ((uint16_t)v * (255 - ((uint16_t)s * (255 - remainder) >> 8))) >> 8;
  uint8_t r, g, b;
  switch (region) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void pickRandomColors() {
  uint16_t hue1 = esp_random() % 360;
  uint16_t hue2 = (hue1 + 120 + esp_random() % 120) % 360;
  fgColor = hsvToRgb565(hue1, 255, 255);
  bgColor = hsvToRgb565(hue2, 255, 80);
  Serial.printf("Colors: hue %u/%u\n", hue1, hue2);
}

// ---- Bit-RLE decoder → RGB565 ----
void decode_bit_rle_to_rgb565(const uint8_t *rle, size_t rleLen,
                               uint16_t *out, size_t totalPixels) {
  if (rleLen < 1) return;
  uint8_t curBit = rle[0];
  bool inv = invertColors;
  uint16_t fg = fgColor;
  uint16_t bg = bgColor;
  if (inv) { uint16_t tmp = fg; fg = bg; bg = tmp; }

  size_t pixel = 0;
  size_t pos = 1;
  while (pos + 1 < rleLen && pixel < totalPixels) {
    uint16_t runLen = rle[pos] | (rle[pos + 1] << 8);
    pos += 2;
    uint16_t color = curBit ? fg : bg;
    size_t end = pixel + runLen;
    if (end > totalPixels) end = totalPixels;
    for (size_t i = pixel; i < end; i++) out[i] = color;
    pixel = end;
    curBit = 1 - curBit;
  }
  for (size_t i = pixel; i < totalPixels; i++) out[i] = bgColor;
}

// ---- Glitch effect (unused, but kept) ----
void apply_glitch(uint16_t *buf, size_t pixels) {
  size_t n = pixels / 8;
  for (size_t i = 0; i < n; i++) {
    buf[esp_random() % pixels] ^= 0xFFFF;
  }
}

// ---- Smooth angle update (not used without IMU, but kept) ----
void updateSmoothAngle(float targetDeg) {
  float diff = targetDeg - smoothAngle;
  while (diff > 180.0f)  diff -= 360.0f;
  while (diff < -180.0f) diff += 360.0f;
  smoothAngle += diff * ANGLE_SMOOTHING;
  while (smoothAngle >= 360.0f) smoothAngle -= 360.0f;
  while (smoothAngle < 0.0f)    smoothAngle += 360.0f;
}

void errorHold(const char *msg) {
  Serial.printf("ERROR: %s\n", msg);
  M5.Lcd.fillScreen(TFT_RED);
  M5.Lcd.setTextColor(TFT_WHITE);
  M5.Lcd.setCursor(10, 10);
  M5.Lcd.setTextSize(1);
  M5.Lcd.println(msg);
  while (true) { M5.update(); delay(1000); }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Lcd.setRotation(3);                // landscape, USB on right
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setTextColor(TFT_WHITE);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(0, 0);

  Serial.begin(115200);
  Serial.println("Bad Apple starting...");

  if (!LittleFS.begin()) errorHold("LittleFS mount failed");

  // ---- Read video header + index ----
  M5.Lcd.println("Loading video...");
  {
    File vf = LittleFS.open(VIDEO_FILE, "r");
    if (!vf) errorHold("Missing video file");
    FileHeader hdr;
    vf.read((uint8_t *)&hdr, sizeof(hdr));
    vidW = hdr.width;
    vidH = hdr.height;
    totalFrames = hdr.total_frames;
    vidFps = hdr.fps;
    Serial.printf("Video: %ux%u, %u frames, %u fps\n", vidW, vidH, totalFrames, vidFps);

    size_t indexSize = totalFrames * sizeof(uint32_t);
    frameIndex = (uint32_t *)malloc(indexSize);        // use normal RAM
    if (!frameIndex) { vf.close(); errorHold("OOM: index"); }
    vf.read((uint8_t *)frameIndex, indexSize);
    frameDataStart = sizeof(FileHeader) + indexSize;
    vf.close();
  }

  // ---- Allocate buffers in normal RAM ----
  size_t pixels = (size_t)vidW * vidH;
  rleBuf = (uint8_t *)malloc(MAX_RLE_SIZE);
  rgb565Buf = (uint16_t *)malloc(pixels * 2);
  if (!rleBuf || !rgb565Buf) errorHold("OOM: buffers");

  // ---- Create sprites (use normal RAM) ----
  canvas.setPsram(false);
  canvas.setColorDepth(16);
  if (!canvas.createSprite(DISP_W, DISP_H)) errorHold("OOM: canvas sprite");

  videoSprite.setPsram(false);
  videoSprite.setColorDepth(16);
  if (!videoSprite.createSprite(vidW, vidH)) errorHold("OOM: video sprite");

  // ---- Set fixed rotation angle to fill screen (90°) ----
  smoothAngle = 90.0f;   // rotate video 90° to match landscape display

  M5.Lcd.fillScreen(TFT_BLACK);
  Serial.println("Ready.");
}

void loop() {
  File vf = LittleFS.open(VIDEO_FILE, "r");
  if (!vf) { errorHold("Cannot open video"); return; }

  size_t pixels = (size_t)vidW * vidH;
  uint32_t frameDelay = 1000 / vidFps;

  bool btnALongHandled = false;

  for (uint32_t frameIdx = 0; frameIdx < totalFrames; frameIdx++) {
    uint32_t frameStart = millis();
    M5.update();

    // ---- BtnA: long = pause, short = invert ----
    if (M5.BtnA.pressedFor(600) && !btnALongHandled) {
      btnALongHandled = true;
      paused = !paused;
      if (paused) {
        Serial.println("PAUSED");
      } else {
        Serial.println("RESUMED");
      }
    }
    if (M5.BtnA.wasReleased()) {
      if (!btnALongHandled) {
        invertColors = !invertColors;
        Serial.printf("Invert: %s\n", invertColors ? "ON" : "OFF");
      }
      btnALongHandled = false;
    }

    // ---- BtnB: random contrasting colors ----
    if (M5.BtnB.wasPressed()) {
      pickRandomColors();
    }

    // ---- Pause loop ----
    while (paused) {
      M5.update();
      if (M5.BtnA.pressedFor(600) && !btnALongHandled) {
        btnALongHandled = true;
        paused = false;
        Serial.println("RESUMED");
      }
      if (M5.BtnA.wasReleased()) btnALongHandled = false;
      delay(30);
    }

    // ---- Read RLE frame ----
    size_t frameOffset = frameIndex[frameIdx];
    size_t nextOffset = (frameIdx + 1 < totalFrames)
                            ? frameIndex[frameIdx + 1]
                            : (vf.size() - frameDataStart);
    size_t rleSize = nextOffset - frameOffset;
    if (rleSize > MAX_RLE_SIZE) rleSize = MAX_RLE_SIZE;

    vf.seek(frameDataStart + frameOffset);
    if (vf.read(rleBuf, rleSize) != rleSize) break;

    // ---- Decode ----
    decode_bit_rle_to_rgb565(rleBuf, rleSize, rgb565Buf, pixels);

    // ---- Render: copy to sprite → rotate into canvas → push to LCD ----
    videoSprite.pushImage(0, 0, vidW, vidH, rgb565Buf);
    canvas.fillSprite(TFT_BLACK);
    videoSprite.pushRotateZoom(&canvas,
                                DISP_W / 2, DISP_H / 2,
                                smoothAngle,
                                1.0f, 1.0f);
    canvas.pushSprite(&M5.Lcd, 0, 0);

    // ---- Frame timing ----
    uint32_t elapsed = millis() - frameStart;
    if (elapsed < frameDelay) delay(frameDelay - elapsed);
  }

  vf.close();
  canvas.fillSprite(TFT_BLACK);
  canvas.pushSprite(&M5.Lcd, 0, 0);
  delay(1000);
}