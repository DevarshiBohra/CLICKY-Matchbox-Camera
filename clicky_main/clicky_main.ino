#include <Update.h>
#include <Arduino.h>
#include <SPI.h>
#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <TFT_eSPI.h>
#include "driver/gpio.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>

// *** OTA credentials
#define OTA_USERNAME  "ADD_USERNAME"
#define OTA_PASSWORD  "ADD_PASSWORD"

// ===================== CAMERA PINS (AI Thinker) =====================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ===================== OTHER PINS =====================
#define BUTTON_PIN   3
#define TFT_CS_PIN  12

// ===================== WIFI SETTINGS =====================
#define AP_SSID     "CLICKY"
#define AP_PASSWORD ""
#define DNS_PORT    53
#define HTTP_PORT   80

// ===================== HOLD THRESHOLDS (ms) =====================
#define TOGGLE_HOLD_MS  2000
#define WIFI_HOLD_MS    5000

// ===================== DOUBLE PRESS =====================
#define DOUBLE_PRESS_MS  500   // max gap between two presses to count as double
#define STOP_REC_DEBOUNCE_MS 300  // ignore button for this long after recording starts

// ===================== EEPROM =====================
#define EEPROM_SIZE        4          // 4 bytes: effect + magic + photo index (2 bytes)
#define EEPROM_ADDR_EFFECT 0
#define EEPROM_MAGIC_ADDR  1
#define EEPROM_MAGIC_VAL   0xA5
#define EEPROM_ADDR_IDX_LO 2         // photo index low byte
#define EEPROM_ADDR_IDX_HI 3         // photo index high byte

// ===================== AVI HEADER CONSTANTS =====================
#define AVI_HEADER_SIZE   252

// ===================== DISPLAY =====================
TFT_eSPI tft = TFT_eSPI();

// ===================== GLOBALS =====================
WebServer server(HTTP_PORT);
DNSServer dnsServer;
bool wifiModeActive  = false;
bool grayscaleActive = false;

// ===================== FORWARD DECLARATIONS =====================
bool initCameraForPreview();
bool initCameraForCapture();
bool initCameraForVideo();
void captureAndSave();
void saveImage(String path, camera_fb_t* fb);
void showMessage(const char* msg, uint16_t bgColor = TFT_BLACK, uint16_t textColor = TFT_WHITE);
void applyEffect(bool grayscale);
void loadEffectFromEEPROM();
void saveEffectToEEPROM(bool grayscale);
uint16_t loadPhotoIndex();
void savePhotoIndex(uint16_t idx);
void migratePhotoIndex();
void showEffectBanner();
void recordVideo();
void writeAviHeader(File &file, uint32_t frameCount, uint32_t totalFrameBytes, uint32_t fps);
void releaseSpiPins();
void reinitDisplay();

void handleOTAPage();
void handleOTAUpload();
void startWifiMode();
void stopWifiMode();
void handleRoot();
void handleList();
void handleDownload();
void handleDelete();
void handleDeleteAll();
void handleStop();
void handleNotFound();
void handleCaptivePortal();


// ===================== EEPROM: EFFECT =====================
void loadEffectFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t magic = EEPROM.read(EEPROM_MAGIC_ADDR);
  if (magic != EEPROM_MAGIC_VAL) {
    EEPROM.write(EEPROM_ADDR_EFFECT, 0);
    EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
    EEPROM.write(EEPROM_ADDR_IDX_LO, 0);
    EEPROM.write(EEPROM_ADDR_IDX_HI, 0);
    EEPROM.commit();
    grayscaleActive = false;
  } else {
    grayscaleActive = (EEPROM.read(EEPROM_ADDR_EFFECT) == 1);
  }
  EEPROM.end();
  Serial.printf("EEPROM: effect = %s\n", grayscaleActive ? "GRAYSCALE" : "NORMAL");
}

void saveEffectToEEPROM(bool grayscale) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_ADDR_EFFECT, grayscale ? 1 : 0);
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
  EEPROM.commit();
  EEPROM.end();
  Serial.printf("EEPROM: saved effect = %s\n", grayscale ? "GRAYSCALE" : "NORMAL");
}

// ===================== EEPROM: PHOTO INDEX =====================
uint16_t loadPhotoIndex() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t magic = EEPROM.read(EEPROM_MAGIC_ADDR);
  uint16_t idx = 1;
  if (magic == EEPROM_MAGIC_VAL) {
    idx = EEPROM.read(EEPROM_ADDR_IDX_LO) | (EEPROM.read(EEPROM_ADDR_IDX_HI) << 8);
    if (idx < 1) idx = 1;
  }
  EEPROM.end();
  return idx;
}

void savePhotoIndex(uint16_t idx) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_ADDR_IDX_LO, idx & 0xFF);
  EEPROM.write(EEPROM_ADDR_IDX_HI, (idx >> 8) & 0xFF);
  EEPROM.commit();
  EEPROM.end();
  Serial.printf("EEPROM: saved photo index = %u\n", idx);
}

// ===================== ONE-TIME MIGRATION =====================
// Scans SD once on first boot after OTA to find the highest existing
// pic index and writes it to EEPROM. Never runs again after that.
void migratePhotoIndex() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t magic  = EEPROM.read(EEPROM_MAGIC_ADDR);
  uint16_t stored = EEPROM.read(EEPROM_ADDR_IDX_LO) |
                   (EEPROM.read(EEPROM_ADDR_IDX_HI) << 8);
  EEPROM.end();

  // If magic is valid and index has been set, nothing to do
  if (magic == EEPROM_MAGIC_VAL && stored > 0) return;

  Serial.println("First boot: scanning SD for highest pic index...");

  // Mount in 1-bit for this one-time scan (display still may be active)
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("Migration: SD mount failed — index will start at 1.");
    savePhotoIndex(1);
    return;
  }

  uint16_t maxIdx = 0;
  File root = SD_MMC.open("/");
  while (true) {
    File f = root.openNextFile();
    if (!f) break;
    String name = f.name();
    f.close();
    // Match both "/pic123.jpg" and "pic123.jpg"
    if (name.indexOf("pic") >= 0 &&
        (name.endsWith(".jpg") || name.endsWith(".JPG"))) {
      // Strip path, prefix and extension to get the number
      int picPos = name.indexOf("pic");
      int dotPos = name.lastIndexOf('.');
      if (picPos >= 0 && dotPos > picPos) {
        String numStr = name.substring(picPos + 3, dotPos);
        uint16_t n = (uint16_t)numStr.toInt();
        if (n > maxIdx) maxIdx = n;
      }
    }
  }
  root.close();
  SD_MMC.end();

  savePhotoIndex(maxIdx + 1);
  Serial.printf("Migration done. Next photo index = %u\n", maxIdx + 1);
}


// ===================== APPLY EFFECT =====================
void applyEffect(bool grayscale) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  s->set_special_effect(s, grayscale ? 2 : 0);
  s->set_hmirror(s, 1);
}

// ===================== RELEASE SPI PINS FOR SD_MMC 4-BIT =====================
void releaseSpiPins() {
  SPI.end();
  pinMode(2,  INPUT_PULLUP);
  pinMode(4,  INPUT_PULLUP);
  pinMode(12, INPUT_PULLUP);
  pinMode(13, INPUT_PULLUP);
  delay(50);
  gpio_reset_pin(GPIO_NUM_2);
  gpio_set_pull_mode(GPIO_NUM_2, GPIO_PULLUP_ONLY);
  delay(30);
}

// ===================== REINIT DISPLAY =====================
void reinitDisplay() {
  digitalWrite(TFT_CS_PIN, HIGH);
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
}


// ===================== AVI HEADER WRITER =====================
void writeAviHeader(File &file, uint32_t frameCount, uint32_t totalFrameBytes, uint32_t fps) {
  auto w32 = [&](uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v), (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) };
    file.write(b, 4);
  };
  auto w16 = [&](uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v), (uint8_t)(v>>8) };
    file.write(b, 2);
  };
  auto wcc = [&](const char* cc) {
    file.write((const uint8_t*)cc, 4);
  };

  uint32_t moviDataSize   = totalFrameBytes + frameCount * 8;
  uint32_t riffSize       = AVI_HEADER_SIZE - 8 + moviDataSize;
  uint32_t usPerFrame     = (fps > 0) ? (1000000 / fps) : 33333;
  uint32_t maxBytesPerSec = totalFrameBytes > 0 ? (totalFrameBytes * fps / (frameCount > 0 ? frameCount : 1)) : 50000;

  const uint32_t W = 160;
  const uint32_t H = 120;

  wcc("RIFF");                         // 0
  w32(riffSize);                       // 4
  wcc("AVI ");                         // 8

  wcc("LIST");                         // 12
  w32(192);                            // 16
  wcc("hdrl");                         // 20

  wcc("avih");                         // 24
  w32(56);                             // 28
  w32(usPerFrame);                     // 32
  w32(maxBytesPerSec);                 // 36
  w32(0);                              // 40
  w32(0x10);                           // 44
  w32(frameCount);                     // 48
  w32(0);                              // 52
  w32(1);                              // 56
  w32(W * H * 3);                      // 60
  w32(W);                              // 64
  w32(H);                              // 68
  w32(0); w32(0); w32(0); w32(0);     // 72

  wcc("LIST");                         // 88
  w32(116);                            // 92
  wcc("strl");                         // 96

  wcc("strh");                         // 100
  w32(56);                             // 104
  wcc("vids");                         // 108
  wcc("MJPG");                         // 112
  w32(0);                              // 116
  w16(0);                              // 120
  w16(0);                              // 122
  w32(0);                              // 124
  w32(1);                              // 128
  w32(fps);                            // 132
  w32(0);                              // 136
  w32(frameCount);                     // 140
  w32(W * H * 3);                      // 144
  w32(0);                              // 148
  w32(0);                              // 152
  w16(0); w16(0); w16(W); w16(H);     // 156

  wcc("strf");                         // 164
  w32(40);                             // 168
  w32(40);                             // 172
  w32(W);                              // 176
  w32(H);                              // 180
  w16(1);                              // 184
  w16(24);                             // 186
  wcc("MJPG");                         // 188
  w32(W * H * 3);                      // 192
  w32(0);                              // 196
  w32(0);                              // 200
  w32(0);                              // 204
  w32(0);                              // 208

  wcc("LIST");                         // 212
  w32(moviDataSize + 4);               // 216
  wcc("movi");                         // 220
}


// ===================== VIDEO RECORDING =====================
void recordVideo() {
  const uint32_t TARGET_FPS = 20;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("● REC", 30, 22);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Single press to stop", 4, 55);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(grayscaleActive ? "Mode: GRAYSCALE" : "Mode: COLOUR", 4, 68);
  delay(2000);

  esp_camera_deinit();
  delay(50);

  releaseSpiPins();

  if (!SD_MMC.begin("/sdcard", false)) {   // 4-bit for video
    Serial.println("SD 4-bit mount failed! Restarting.");
    esp_restart();
  }
  Serial.println("SD mounted in 4-bit mode for video.");

  if (!initCameraForVideo()) {
    Serial.println("Camera video init failed! Restarting.");
    SD_MMC.end();
    esp_restart();
  }

  int vidIndex = 1;
  String vidPath;
  while (true) {
    vidPath = "/vid" + String(vidIndex) + ".avi";
    if (!SD_MMC.exists(vidPath)) break;
    vidIndex++;
  }
  Serial.println("Recording to: " + vidPath);

  File vidFile = SD_MMC.open(vidPath.c_str(), FILE_WRITE);
  if (!vidFile) {
    Serial.println("Failed to open video file! Restarting.");
    SD_MMC.end();
    esp_restart();
  }

  writeAviHeader(vidFile, 0, 0, TARGET_FPS);

  uint32_t frameCount      = 0;
  uint32_t totalFrameBytes = 0;
  unsigned long frameInterval = 1000 / TARGET_FPS;
  unsigned long lastFrameTime = millis();

  for (int i = 0; i < 3; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(80);
  }

  unsigned long recStartTime = millis();
  bool stopRequested = false;

  Serial.println("Recording started.");

  while (!stopRequested) {

    unsigned long now = millis();
    if (now - lastFrameTime < frameInterval) {
      if ((now - recStartTime > STOP_REC_DEBOUNCE_MS) &&
          digitalRead(BUTTON_PIN) == LOW) {
        delay(40);
        if (digitalRead(BUTTON_PIN) == LOW) {
          while (digitalRead(BUTTON_PIN) == LOW);
          stopRequested = true;
        }
      }
      delay(2);
      continue;
    }
    lastFrameTime = millis();

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Frame grab failed — skipping.");
      continue;
    }

    uint32_t frameLen = fb->len;
    vidFile.write((const uint8_t*)"00dc", 4);
    uint8_t lenBytes[4] = {
      (uint8_t)(frameLen),
      (uint8_t)(frameLen >> 8),
      (uint8_t)(frameLen >> 16),
      (uint8_t)(frameLen >> 24)
    };
    vidFile.write(lenBytes, 4);
    vidFile.write(fb->buf, fb->len);

    if (frameLen & 1) {
      uint8_t pad = 0;
      vidFile.write(&pad, 1);
    }

    totalFrameBytes += frameLen;
    frameCount++;
    esp_camera_fb_return(fb);

    if (!vidFile) {
      Serial.println("File write error — stopping.");
      stopRequested = true;
    }

    if ((millis() - recStartTime > STOP_REC_DEBOUNCE_MS) &&
        digitalRead(BUTTON_PIN) == LOW) {
      delay(40);
      if (digitalRead(BUTTON_PIN) == LOW) {
        while (digitalRead(BUTTON_PIN) == LOW);
        stopRequested = true;
      }
    }
  }

  Serial.printf("Recording stopped. Frames: %u  Total bytes: %u\n", frameCount, totalFrameBytes);

  vidFile.seek(0);
  writeAviHeader(vidFile, frameCount, totalFrameBytes, TARGET_FPS);
  vidFile.close();

  Serial.println("AVI file finalised. Restarting.");
  SD_MMC.end();

  delay(300);
  esp_restart();
}


// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== CLICKY: Preview + Capture + Video + WiFi ===");

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(TFT_CS_PIN, OUTPUT);
  digitalWrite(TFT_CS_PIN, HIGH);

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  showMessage("Initializing...");

  loadEffectFromEEPROM();

  // One-time migration: finds highest existing pic index on SD
  // and stores it in EEPROM so future captures are O(1).
  // Runs only on first boot after flashing this firmware.
  migratePhotoIndex();

  if (!initCameraForPreview()) {
    showMessage("Camera FAILED!", TFT_RED, TFT_WHITE);
    while (true) delay(1000);
  }

  applyEffect(grayscaleActive);

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(grayscaleActive ? TFT_LIGHTGREY : TFT_GREEN, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString(grayscaleActive ? "Effect: GRAYSCALE" : "Effect: NORMAL", 6, 35);
  delay(900);
  tft.fillScreen(TFT_BLACK);

  Serial.println("Ready. Short=capture | Double=video | 3-5s=toggle | 5s+=WiFi");
}


// ===================== LOOP =====================
void loop() {

  // ---- WiFi mode ----
  if (wifiModeActive) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  // ---- Button handling ----
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {

      unsigned long pressStart = millis();

      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      tft.setTextSize(1);
      tft.drawString("Capture mode", 4, 20);
      tft.drawRect(4, 50, 152, 10, TFT_DARKGREY);

      int lastPhase = 0;

      while (digitalRead(BUTTON_PIN) == LOW) {
        unsigned long held = millis() - pressStart;

        int phase;
        uint16_t barColor;
        if (held < TOGGLE_HOLD_MS) {
          phase    = 0;
          barColor = TFT_YELLOW;
        } else if (held < WIFI_HOLD_MS) {
          phase    = 1;
          barColor = TFT_CYAN;
        } else {
          phase    = 2;
          barColor = TFT_GREEN;
        }

        if (phase != lastPhase) {
          tft.fillRect(0, 0, 160, 45, TFT_BLACK);
          if (phase == 1) {
            tft.setTextColor(TFT_CYAN, TFT_BLACK);
            tft.drawString("Toggle Effect", 4, 20);
            tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
            tft.drawString(grayscaleActive ? "-> NORMAL" : "-> GRAYSCALE", 4, 32);
          } else if (phase == 2) {
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.drawString("WiFi Mode", 4, 20);
          }
          lastPhase = phase;
        }

        int barWidth = (int)map(
          min(held, (unsigned long)WIFI_HOLD_MS),
          0, WIFI_HOLD_MS, 0, 148
        );
        tft.fillRect(5, 51, 150, 8, TFT_BLACK);
        tft.fillRect(5, 51, barWidth, 8, barColor);

        if (held >= WIFI_HOLD_MS) {
          while (digitalRead(BUTTON_PIN) == LOW);
          delay(100);
          startWifiMode();
          return;
        }
        delay(30);
      }

      unsigned long held = millis() - pressStart;

      if (held >= TOGGLE_HOLD_MS && held < WIFI_HOLD_MS) {
        // --- Toggle grayscale ---
        grayscaleActive = !grayscaleActive;
        saveEffectToEEPROM(grayscaleActive);
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("Effect toggled!", 4, 20);
        tft.setTextColor(grayscaleActive ? TFT_LIGHTGREY : TFT_GREEN, TFT_BLACK);
        tft.drawString(grayscaleActive ? "GRAYSCALE" : "NORMAL", 4, 35);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawString("Restarting...", 4, 50);
        delay(1200);
        esp_restart();

      } else if (held < TOGGLE_HOLD_MS) {
        // --- Short press: single or first of a double ---
        unsigned long releaseTime = millis();
        bool secondPress = false;

        while (millis() - releaseTime < DOUBLE_PRESS_MS) {
          if (digitalRead(BUTTON_PIN) == LOW) {
            delay(40);
            if (digitalRead(BUTTON_PIN) == LOW) {
              secondPress = true;
              while (digitalRead(BUTTON_PIN) == LOW);
              delay(50);
              break;
            }
          }
          delay(5);
        }

        if (secondPress) {
          // ========== DOUBLE PRESS — start video recording ==========
          Serial.println("Double press — starting video recording");
          esp_camera_deinit();
          delay(50);
          tft.fillScreen(TFT_BLACK);
          recordVideo();
          // never returns — ends with esp_restart()

        } else {
          // ========== SINGLE PRESS — capture photo ==========
          Serial.println("Single press — capturing photo");
          showMessage("Capturing...", TFT_BLUE, TFT_WHITE);
          delay(100);

          // Release SPI/display pins so SD_MMC can use them in 4-bit mode
          digitalWrite(TFT_CS_PIN, HIGH);
          SPI.end();
          pinMode(2,  INPUT_PULLUP);
          pinMode(4,  INPUT_PULLUP);
          pinMode(12, INPUT_PULLUP);
          pinMode(13, INPUT_PULLUP);
          delay(30);   // tighter settle — 30 ms is enough after SPI.end()

          esp_camera_deinit();
          if (initCameraForCapture()) {
            applyEffect(grayscaleActive);
            captureAndSave();  // restarts on success — never returns
          } else {
            Serial.println("Camera (capture) init failed.");
          }
          esp_camera_deinit();

          // Only reached if SD mount failed inside captureAndSave()
          gpio_reset_pin(GPIO_NUM_2);
          delay(20);
          tft.init();
          tft.setRotation(3);
          tft.fillScreen(TFT_BLACK);
          showMessage("SD Failed!", TFT_RED, TFT_BLACK);
          delay(800);
          tft.fillScreen(TFT_BLACK);

          if (!initCameraForPreview()) {
            showMessage("Camera FAILED!", TFT_RED, TFT_WHITE);
            while (true) delay(1000);
          }
          applyEffect(grayscaleActive);

          while (digitalRead(BUTTON_PIN) == LOW);
          delay(200);
        }
      }
    }
  }

  // ---- Live preview ----
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return;

  tft.startWrite();
  tft.setSwapBytes(false);
  tft.pushImage(0, 0, fb->width, 80, (uint16_t*)fb->buf + 20 * 160);
  tft.endWrite();

  esp_camera_fb_return(fb);
}


// ===================== WIFI MODE: START =====================
void handleCaptivePortal() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void startWifiMode() {
  Serial.println("Entering WiFi mode...");

  esp_camera_deinit();
  delay(100);

  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(AP_SSID, strlen(AP_PASSWORD) > 0 ? AP_PASSWORD : nullptr);
  delay(500);
  Serial.printf("AP %s | IP: %s\n", apOk ? "OK" : "FAILED", WiFi.softAPIP().toString().c_str());

  tft.fillScreen(TFT_NAVY);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("WiFi Active!", 46, 10);
  tft.setTextColor(TFT_CYAN, TFT_NAVY);
  tft.drawString("SSID:", 10, 30);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString(AP_SSID, 10, 41);
  tft.setTextColor(TFT_CYAN, TFT_NAVY);
  tft.drawString("Open browser:", 78, 30);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("192.168.4.1", 78, 41);
  if (!apOk) {
    tft.setTextColor(TFT_RED, TFT_NAVY);
    tft.drawString("AP FAILED!", 7, 57);
  }

  // Release SPI pins — 1-bit SD for WiFi mode (display was still alive above)
  SPI.end();
  pinMode(2,  INPUT_PULLUP);
  pinMode(4,  INPUT_PULLUP);
  pinMode(12, INPUT_PULLUP);
  pinMode(13, INPUT_PULLUP);
  delay(100);
  gpio_reset_pin(GPIO_NUM_2);
  gpio_set_pull_mode(GPIO_NUM_2, GPIO_PULLUP_ONLY);
  delay(50);

  // 1-bit mode intentional here — display teardown happens just above,
  // and WiFi gallery only reads files (no high write speed needed).
  bool sdOk = SD_MMC.begin("/sdcard", true);
  if (!sdOk) {
    Serial.println("SD mount failed — gallery will be empty.");
  } else {
    Serial.println("SD mounted OK (1-bit, WiFi mode).");
  }

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  server.on("/hotspot-detect.html",       HTTP_GET, handleCaptivePortal);
  server.on("/library/test/success.html", HTTP_GET, handleCaptivePortal);
  server.on("/generate_204",              HTTP_GET, handleCaptivePortal);
  server.on("/connecttest.txt",           HTTP_GET, handleCaptivePortal);
  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/list",      HTTP_GET,  handleList);
  server.on("/download",  HTTP_GET,  handleDownload);
  server.on("/delete",    HTTP_POST, handleDelete);
  server.on("/deleteall", HTTP_POST, handleDeleteAll);
  server.on("/stop",      HTTP_POST, handleStop);
  server.on("/update",    HTTP_GET,  handleOTAPage);
  server.onNotFound(handleNotFound);
  handleOTAUpload();

  const char* headerKeys[] = {"User-Agent"};
  server.collectHeaders(headerKeys, 1);

  server.begin();
  Serial.println("Web server started.");

  wifiModeActive = true;
}


// ===================== WIFI MODE: STOP =====================
void stopWifiMode() {
  Serial.println("Stopping WiFi mode...");
  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  SD_MMC.end();
  wifiModeActive = false;
  delay(500);
  esp_restart();
}


// ===================== WEB PAGE =====================
static const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CLICKY Gallery</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,sans-serif;background:#111;color:#eee;padding:16px;padding-bottom:72px}
  h1{font-size:1.3rem;margin-bottom:4px;color:#fff}
  .sub{font-size:.8rem;color:#888;margin-bottom:8px}
  .tagline{font-size:.8rem;color:#6366f1;font-style:italic;margin-bottom:16px;letter-spacing:.03em}
  .toolbar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:16px;align-items:center}
  button{padding:8px 16px;border:none;border-radius:8px;cursor:pointer;font-size:.85rem;font-weight:600}
  .btn-dl{background:#2563eb;color:#fff}
  .btn-del{background:#dc2626;color:#fff}
  .btn-stop{background:#374151;color:#eee}
  .btn-all{background:#7c3aed;color:#fff}
  button:disabled{opacity:.4;cursor:not-allowed}
  .sel-count{font-size:.85rem;color:#aaa;margin-left:4px}
  .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:12px}
  .card{background:#1e1e1e;border-radius:10px;overflow:hidden;cursor:pointer;transition:outline .1s}
  .card:has(input:checked){outline:3px solid #2563eb}
  .card img{width:100%;aspect-ratio:4/3;object-fit:cover;display:block;background:#333}
  .card video{width:100%;aspect-ratio:4/3;object-fit:cover;display:block;background:#222}
  .card-footer{display:flex;align-items:center;gap:6px;padding:6px 8px}
  .card-footer input{width:16px;height:16px;accent-color:#2563eb;flex-shrink:0}
  .fname{font-size:.75rem;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#ccc}
  .badge{font-size:.65rem;background:#7c3aed;color:#fff;border-radius:4px;padding:1px 5px;flex-shrink:0}
  .empty{color:#666;text-align:center;padding:40px;grid-column:1/-1}
  .toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%);background:#22c55e;color:#fff;
         padding:10px 20px;border-radius:10px;font-weight:600;opacity:0;transition:opacity .4s;pointer-events:none;z-index:1001}
  .toast.show{opacity:1}
  .credit{position:fixed;bottom:16px;left:16px;font-size:.75rem;color:#bbb;line-height:1.5;pointer-events:none;
          background:rgba(0,0,0,.65);padding:6px 10px;border-radius:8px;z-index:1000;backdrop-filter:blur(3px)}
  @media(prefers-color-scheme:light){
    body{background:#f3f4f6;color:#111}
    .card{background:#fff}.fname{color:#444}.empty{color:#999}
    .btn-stop{background:#e5e7eb;color:#333}
    .credit{color:#444;background:rgba(255,255,255,.8)}
  }
</style>
</head>
<body>
<h1>CLICKY Gallery</h1>
<p class="sub" id="sub">Loading...</p>
<p class="tagline">A photo is forever, diamonds ke advertisment se churai hue line</p>
<div class="toolbar">
  <button class="btn-dl"  id="btn-dl"  onclick="downloadSelected()" disabled>Download selected</button>
  <button class="btn-del" id="btn-del" onclick="deleteSelected()"   disabled>Delete selected</button>
  <button class="btn-all" id="btn-selall" onclick="toggleSelectAll()">Select all</button>
  <button class="btn-stop" onclick="stopWifi()">Stop WiFi</button>
  <span class="sel-count" id="sel-count"></span>
</div>
<div class="grid" id="grid"></div>
<div class="toast" id="toast"></div>
<div class="credit">Made by:<br>Devarshi Bohra</div>
<script>
let files=[];
async function load(){
  const r=await fetch('/list');
  files=await r.json();
  render();
}
function isVideo(f){return f.endsWith('.avi')||f.endsWith('.AVI');}
function render(){
  const grid=document.getElementById('grid');
  const photos=files.filter(f=>!isVideo(f)).length;
  const videos=files.filter(f=>isVideo(f)).length;
  let sub=files.length+' file'+(files.length!==1?'s':'')+' on SD card';
  if(photos&&videos) sub=photos+' photo'+(photos!==1?'s':'')+' · '+videos+' video'+(videos!==1?'s':'');
  document.getElementById('sub').textContent=sub;
  if(!files.length){grid.innerHTML='<p class="empty">No files found.</p>';}
  else{
    grid.innerHTML=files.map(f=>`
      <label class="card">
        ${isVideo(f)
          ? `<video src="/download?f=${encodeURIComponent(f)}" preload="metadata" controls muted playsinline></video>`
          : `<img src="/download?f=${encodeURIComponent(f)}" alt="${f}" loading="lazy">`}
        <div class="card-footer">
          <input type="checkbox" value="${f}" onchange="updateToolbar()">
          ${isVideo(f)?'<span class="badge">VID</span>':''}
          <span class="fname">${f}</span>
        </div>
      </label>`).join('');
  }
  updateToolbar();
}
function selected(){return[...document.querySelectorAll('input[type=checkbox]:checked')].map(c=>c.value);}
function updateToolbar(){
  const s=selected();const any=s.length>0;
  document.getElementById('btn-dl').disabled=!any;
  document.getElementById('btn-del').disabled=!any;
  document.getElementById('sel-count').textContent=any?s.length+' selected':'';
  const boxes=[...document.querySelectorAll('input[type=checkbox]')];
  const btnSel=document.getElementById('btn-selall');
  if(btnSel) btnSel.textContent=(boxes.length>0&&boxes.every(c=>c.checked))?'Deselect all':'Select all';
}
function toggleSelectAll(){
  const boxes=[...document.querySelectorAll('input[type=checkbox]')];
  if(!boxes.length)return;
  const allChecked=boxes.every(c=>c.checked);
  boxes.forEach(c=>c.checked=!allChecked);
  updateToolbar();
}
function toast(msg,color='#22c55e'){
  const el=document.getElementById('toast');
  el.textContent=msg;el.style.background=color;
  el.classList.add('show');setTimeout(()=>el.classList.remove('show'),2500);
}
function isIOS(){return/iPad|iPhone|iPod/.test(navigator.userAgent)&&!window.MSStream;}
function downloadSelected(){
  const sel=selected();if(!sel.length)return;
  if(isIOS()){sel.forEach((f,i)=>setTimeout(()=>window.open('/download?f='+encodeURIComponent(f),'_blank'),i*400));}
  else{sel.forEach(f=>{const a=document.createElement('a');a.href='/download?f='+encodeURIComponent(f);a.download=f;document.body.appendChild(a);a.click();document.body.removeChild(a);});}
}
async function deleteSelected(){
  const sel=selected();if(!sel.length)return;
  if(!confirm('Delete '+sel.length+' file(s)?'))return;
  const fd=new FormData();sel.forEach(f=>fd.append('files',f));
  const r=await fetch('/delete',{method:'POST',body:fd});
  const j=await r.json();toast('Deleted '+j.deleted+' file(s)');await load();
}
async function stopWifi(){
  if(!confirm('Stop WiFi and return to camera mode?'))return;
  toast('Restarting...','#6366f1');
  await fetch('/stop',{method:'POST'}).catch(()=>{});
}
load();
</script>
</body>
</html>
)rawhtml";


// ===================== OTA PAGE =====================
static const char OTA_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CLICKY — Firmware Update</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,sans-serif;background:#111;color:#eee;
       display:flex;flex-direction:column;align-items:center;
       justify-content:center;min-height:100vh;padding:24px}
  .card{background:#1e1e1e;border-radius:16px;padding:32px;width:100%;max-width:400px;text-align:center}
  h1{font-size:1.2rem;margin-bottom:4px;color:#fff}
  .sub{font-size:.8rem;color:#666;margin-bottom:24px}
  .drop{border:2px dashed #374151;border-radius:12px;padding:32px 16px;
        cursor:pointer;transition:border-color .2s;margin-bottom:16px}
  .drop:hover,.drop.over{border-color:#6366f1}
  .drop input{display:none}
  .drop-icon{font-size:2rem;margin-bottom:8px}
  .drop-label{font-size:.85rem;color:#9ca3af}
  .fname{font-size:.8rem;color:#6366f1;margin-top:6px;min-height:18px}
  button{width:100%;padding:12px;border:none;border-radius:10px;
         background:#6366f1;color:#fff;font-size:.95rem;font-weight:600;
         cursor:pointer;transition:opacity .2s}
  button:disabled{opacity:.4;cursor:not-allowed}
  .bar-wrap{background:#374151;border-radius:8px;height:8px;overflow:hidden;margin-top:16px;display:none}
  .bar{height:100%;background:#6366f1;width:0%;transition:width .2s}
  .status{margin-top:12px;font-size:.85rem;color:#9ca3af;min-height:20px}
  .ok{color:#22c55e}.err{color:#ef4444}
</style>
</head>
<body>
<div class="card">
  <h1>Firmware Update</h1>
  <p class="sub">CLICKY — OTA</p>
  <div class="drop" id="drop" onclick="document.getElementById('fw').click()">
    <div class="drop-icon">📦</div>
    <div class="drop-label">Click or drag a .bin file here</div>
    <div class="fname" id="fname"></div>
    <input type="file" id="fw" accept=".bin">
  </div>
  <button id="btn" disabled onclick="upload()">Flash firmware</button>
  <div class="bar-wrap" id="bar-wrap"><div class="bar" id="bar"></div></div>
  <div class="status" id="status"></div>
</div>
<script>
const drop=document.getElementById('drop');
const fw=document.getElementById('fw');
const btn=document.getElementById('btn');
drop.addEventListener('dragover',e=>{e.preventDefault();drop.classList.add('over')});
drop.addEventListener('dragleave',()=>drop.classList.remove('over'));
drop.addEventListener('drop',e=>{e.preventDefault();drop.classList.remove('over');if(e.dataTransfer.files[0]){fw.files=e.dataTransfer.files;onFile();}});
fw.addEventListener('change',onFile);
function onFile(){if(!fw.files[0])return;document.getElementById('fname').textContent=fw.files[0].name;btn.disabled=false;}
function upload(){
  const file=fw.files[0];if(!file)return;
  btn.disabled=true;
  const wrap=document.getElementById('bar-wrap');
  const bar=document.getElementById('bar');
  const st=document.getElementById('status');
  wrap.style.display='block';st.textContent='Uploading...';
  const xhr=new XMLHttpRequest();
  xhr.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);bar.style.width=p+'%';st.textContent='Uploading: '+p+'%';}};
  xhr.onload=()=>{if(xhr.status===200){st.innerHTML='<span class="ok">✓ Done — board is restarting</span>';}else{st.innerHTML='<span class="err">✗ Upload failed ('+xhr.status+')</span>';btn.disabled=false;}};
  xhr.onerror=()=>{st.innerHTML='<span class="err">✗ Network error</span>';btn.disabled=false;};
  const fd=new FormData();fd.append('firmware',file);
  xhr.open('POST','/update');xhr.send(fd);
}
</script>
</body>
</html>
)rawhtml";


// ===================== HTTP HANDLERS =====================

void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", HTML_PAGE);
}

void handleNotFound() {
  String host = server.hostHeader();
  if (host == "captive.apple.com" || host == "www.apple.com" || host == "apple.com") {
    server.send(200, "text/html",
      "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    return;
  }
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void handleList() {
  File root = SD_MMC.open("/");
  String json = "[";
  bool first = true;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String name = entry.name();
    entry.close();
    bool isJpeg = name.endsWith(".jpg") || name.endsWith(".jpeg") ||
                  name.endsWith(".JPG") || name.endsWith(".JPEG");
    bool isAvi  = name.endsWith(".avi") || name.endsWith(".AVI");
    if (isJpeg || isAvi) {
      if (name.startsWith("/")) name = name.substring(1);
      if (!first) json += ",";
      json += "\"" + name + "\"";
      first = false;
    }
  }
  root.close();
  json += "]";
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "application/json", json);
}

void handleDownload() {
  if (!server.hasArg("f")) { server.send(400, "text/plain", "Missing param"); return; }
  String filename = "/" + server.arg("f");
  File file = SD_MMC.open(filename);
  if (!file) { server.send(404, "text/plain", "Not found"); return; }

  String fname = server.arg("f");
  bool isAvi = fname.endsWith(".avi") || fname.endsWith(".AVI");
  String mime = isAvi ? "video/x-msvideo" : "image/jpeg";

  String disposition = "attachment";
  if (server.hasHeader("User-Agent")) {
    String ua = server.header("User-Agent");
    if (ua.indexOf("iPhone") >= 0 || ua.indexOf("iPad") >= 0 || ua.indexOf("iPod") >= 0) {
      disposition = "inline";
    }
  }
  server.sendHeader("Content-Disposition", disposition + "; filename=\"" + fname + "\"");
  server.sendHeader("Cache-Control", "no-cache");
  server.streamFile(file, mime);
  file.close();
}

void handleDelete() {
  int deleted = 0;
  uint16_t curIdx = loadPhotoIndex();
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "files") {
      String fname = server.arg(i);
      String path = "/" + fname;
      if (SD_MMC.remove(path)) {
        Serial.println("Deleted: " + path);
        deleted++;
        // If the most recently saved photo was just deleted, roll the
        // counter back by one so numbering doesn't skip — a plain string
        // compare against work we're already doing, no extra SD access.
        if (curIdx > 1 && fname == ("pic" + String(curIdx - 1) + ".jpg")) {
          curIdx--;
        }
      }
    }
  }
  if (curIdx != loadPhotoIndex()) savePhotoIndex(curIdx);
  server.send(200, "application/json", "{\"deleted\":" + String(deleted) + "}");
}

void handleDeleteAll() {
  File root = SD_MMC.open("/");
  int deleted = 0;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String name = entry.name();
    entry.close();
    bool isJpeg = name.endsWith(".jpg") || name.endsWith(".jpeg") ||
                  name.endsWith(".JPG") || name.endsWith(".JPEG");
    bool isAvi  = name.endsWith(".avi") || name.endsWith(".AVI");
    if (isJpeg || isAvi) {
      if (!name.startsWith("/")) name = "/" + name;
      if (SD_MMC.remove(name)) deleted++;
    }
  }
  root.close();
  // SD is now confirmed empty of media (we just enumerated every file
  // above) — reset the counter at zero extra cost.
  savePhotoIndex(1);
  server.send(200, "application/json", "{\"deleted\":" + String(deleted) + "}");
}

void handleStop() {
  server.send(200, "text/plain", "Restarting...");
  delay(300);
  stopWifiMode();
}

void handleOTAPage() {
  if (!server.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
    return server.requestAuthentication();
  }
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", OTA_PAGE);
}

void handleOTAUpload() {
  server.on("/update", HTTP_POST,
    []() {
      if (!server.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
        return server.requestAuthentication();
      }
      if (Update.hasError()) {
        server.send(500, "text/plain", "Update failed!");
      } else {
        server.send(200, "text/plain", "OK");
        delay(500);
        esp_restart();
      }
    },
    []() {
      if (!server.authenticate(OTA_USERNAME, OTA_PASSWORD)) return;
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("OTA start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
          Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
          Serial.printf("OTA success: %u bytes\n", upload.totalSize);
        } else {
          Update.printError(Serial);
        }
      }
    }
  );
}


// ===================== CAMERA: PREVIEW MODE =====================
bool initCameraForPreview() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size   = FRAMESIZE_QQVGA;
  config.jpeg_quality = 12;
  config.fb_count     = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) { Serial.printf("Preview cam init failed: 0x%x\n", err); return false; }

  sensor_t* s = esp_camera_sensor_get();
  if (s) { s->set_hmirror(s, 1); s->set_special_effect(s, grayscaleActive ? 2 : 0); }

  Serial.println("Camera: preview mode (160x120 RGB565)");
  return true;
}

// ===================== CAMERA: CAPTURE MODE =====================
bool initCameraForCapture() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    Serial.println("PSRAM → UXGA (1600x1200)");
  } else {
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    Serial.println("No PSRAM → SVGA");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) { Serial.printf("Capture cam init failed: 0x%x\n", err); return false; }

  sensor_t* s = esp_camera_sensor_get();
  if (s) { s->set_hmirror(s, 1); s->set_special_effect(s, grayscaleActive ? 2 : 0); }

  Serial.println("Camera: capture mode ready");
  return true;
}

// ===================== CAMERA: VIDEO MODE =====================
bool initCameraForVideo() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count     = 2;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) { Serial.printf("Video cam init failed: 0x%x\n", err); return false; }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_hmirror(s, 1);
    s->set_special_effect(s, grayscaleActive ? 2 : 0);
    s->set_gain_ctrl(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_whitebal(s, 1);
  }

  Serial.println("Camera: video mode (QVGA JPEG, 2 buffers)");
  return true;
}


// ===================== CAPTURE AND SAVE (PHOTO) =====================
void captureAndSave() {
  Serial.printf("GPIO2 before SD mount: %d\n", digitalRead(2));

  // 4-bit mode: ~2× faster writes vs 1-bit.
  // Safe here because SPI.end() + pin release already happened
  // in the single-press handler before this function is called.
  if (!SD_MMC.begin("/sdcard", false)) {
    Serial.println("SD mount failed!");
    return;
  }
  Serial.println("SD mounted (4-bit).");

  // 3 frame-paced discards — no artificial delay needed.
  // Each fb_get blocks for one full frame period (~67ms at UXGA/15fps).
  // Total: ~200ms, which covers the OV3660's typical 3-4 frame convergence.
  for (int i = 0; i < 3; i++) {
    camera_fb_t* discard = esp_camera_fb_get();
    if (discard) esp_camera_fb_return(discard);
    delay(20);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { Serial.println("Frame capture failed!"); SD_MMC.end(); return; }

  // O(1) index lookup from EEPROM — no SD scan regardless of file count
  uint16_t idx = loadPhotoIndex();

  // Cheap external-deletion guard — O(1), no directory scan.
  // If the photo we believe we saved last (idx-1) is gone, the SD card
  // was almost certainly cleared/edited outside the firmware (card
  // reader, PC, etc.) — reset numbering instead of climbing from a
  // stale value forever.
  if (idx > 1 && !SD_MMC.exists("/pic" + String(idx - 1) + ".jpg")) {
    Serial.printf("Last known photo pic%u.jpg missing — SD changed externally, resetting index.\n", idx - 1);
    idx = 1;
  }

  String path = "/pic" + String(idx) + ".jpg";

  // Fallback safety: if EEPROM ever gets out of sync, walk forward
  // (this loop will almost never execute in normal operation)
  while (SD_MMC.exists(path)) {
    Serial.printf("Index collision at %u — advancing.\n", idx);
    idx++;
    path = "/pic" + String(idx) + ".jpg";
  }

  saveImage(path, fb);
  savePhotoIndex(idx + 1);   // pre-increment so next shot needs no scan

  esp_camera_fb_return(fb);
  SD_MMC.end();
  Serial.println("SD unmounted. Restarting.");
  esp_restart();
}

// ===================== SAVE IMAGE =====================
void saveImage(String path, camera_fb_t* fb) {
  File file = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (!file) { Serial.println("Failed to open file for writing"); return; }
  file.write(fb->buf, fb->len);
  file.close();
  Serial.println("Saved: " + path + " (" + String(fb->len) + " bytes)");
}

// ===================== SHOW MESSAGE ON TFT =====================
void showMessage(const char* msg, uint16_t bgColor, uint16_t textColor) {
  tft.fillScreen(bgColor);
  tft.setTextColor(textColor, bgColor);
  tft.setTextSize(1);
  tft.drawString(msg, 10, 35);
}
