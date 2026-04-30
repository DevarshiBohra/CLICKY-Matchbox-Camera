#pragma once

// ===================== OTA CREDENTIALS =====================
#define OTA_USERNAME  "Add own username"
#define OTA_PASSWORD  "Add own password"

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
#define AP_SSID        "CLICKY"
#define AP_PASSWORD    ""
#define DNS_PORT       53
#define HTTP_PORT      80

// ===================== HOLD THRESHOLDS (ms) =====================
#define TOGGLE_HOLD_MS  3000
#define WIFI_HOLD_MS    5000

// ===================== EEPROM =====================
#define EEPROM_SIZE        2
#define EEPROM_ADDR_EFFECT 0
#define EEPROM_MAGIC_ADDR  1
#define EEPROM_MAGIC_VAL   0xA5