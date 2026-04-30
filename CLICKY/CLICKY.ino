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

#include "config.h"
#include "camera.h"
#include "display.h"
#include "sd_capture.h"
#include "wifi_server.h"
#include "eeprom_helpers.h"

TFT_eSPI tft = TFT_eSPI();
WebServer server(HTTP_PORT);
DNSServer dnsServer;
bool wifiModeActive  = false;
bool grayscaleActive = false;

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32-CAM: Preview + Capture + WiFi + Grayscale ===");

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

  Serial.println("Ready. Short press=capture | 3-5s=toggle effect | Hold 5s=WiFi.");
}

void loop() {
  if (wifiModeActive) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {

      unsigned long pressStart = millis();

      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
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
        grayscaleActive = !grayscaleActive;
        Serial.printf("Effect toggled → %s\n", grayscaleActive ? "GRAYSCALE" : "NORMAL");
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
        Serial.println("Short press — capturing");
        showMessage("Capturing...", TFT_BLUE, TFT_WHITE);
        delay(100);

        digitalWrite(TFT_CS_PIN, HIGH);
        SPI.end();
        gpio_reset_pin(GPIO_NUM_2);
        gpio_set_pull_mode(GPIO_NUM_2, GPIO_PULLUP_ONLY);
        delay(50);

        pinMode(2,  INPUT_PULLUP);
        pinMode(4,  INPUT_PULLUP);
        pinMode(12, INPUT_PULLUP);
        pinMode(13, INPUT_PULLUP);
        delay(50);

        esp_camera_deinit();
        if (initCameraForCapture()) {
          applyEffect(grayscaleActive);
          captureAndSave();
        } else {
          Serial.println("Camera (capture mode) init failed.");
        }
        esp_camera_deinit();

        gpio_reset_pin(GPIO_NUM_2);
        delay(20);
        tft.init();
        tft.setRotation(1);
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

  // Live preview
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return;

  tft.startWrite();
  tft.setSwapBytes(false);
  tft.pushImage(0, 0, fb->width, 80, (uint16_t*)fb->buf + 20 * 160);
  tft.endWrite();

  esp_camera_fb_return(fb);
}