/*
 * CLICKY — ESP32-CAM (AI Thinker): live preview + photo + video + WiFi gallery + OTA
 *
 * What's new in this version
 *  - WiFi gallery: multi-file download is ONE streamed ZIP (no browser throttling, progress bar works)
 *  - Gallery shows full-quality photos (loaded a few at a time); videos show a plain tile
 *  - Gallery server: bigger streaming buffer, WiFi power-save off, image requests limited to 3 at a time
 *  - Numbering: O(1) counters for photos AND videos stored in NVS (Preferences), one-time migration
 *  - Media now lives in /photos and /videos (old files in the SD root are still listed/downloadable)
 *  - Stability: zero-initialised camera config, retries + sensor power-cycle, SD 4-bit -> 1-bit fallback,
 *    JPEG validation, checked writes, task watchdog (auto-reboot on any hang) and a "last stage" log
 *  - AVI header fixed (320x240, correct sizes, real frame rate)
 *  - Faster photo cycle: camera init overlaps the double-press wait, time-based exposure settle,
 *    no banner delay after a capture restart, trimmed delays (see SPECULATIVE_CAPTURE_INIT / CAM_SETTLE_MS)
 *
 * Button:  short = photo | double = video | hold 2-5 s = toggle grayscale | hold 5 s+ = WiFi gallery
 */

#include <Arduino.h>
#include <Update.h>
#include <SPI.h>
#include <vector>
#include <dirent.h>
#include <string.h>
#include <strings.h>
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
#include <EEPROM.h>          // only used ONCE to import settings saved by the older firmware
#include <Preferences.h>
#include "esp_task_wdt.h"
#include "esp_system.h"

// *** OTA credentials
#define OTA_USERNAME  "yourUsername"
#define OTA_PASSWORD  "yourPassword"

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
#define BUTTON_PIN   33               //was 3
#define TFT_CS_PIN  12

// ===================== WIFI SETTINGS =====================
#define AP_SSID         "CLICKY"
#define AP_PASSWORD     ""          // empty = open network. If set, must be 8+ characters.
#define AP_CHANNEL      1
#define AP_MAX_CLIENTS  2           // 2 (not 1) so a phone that dropped uncleanly can't lock you out
#define DNS_PORT        53
#define HTTP_PORT       80

// ===================== BUTTON TIMING (ms) =====================
#define TOGGLE_HOLD_MS        2000
#define WIFI_HOLD_MS          5000
#define DOUBLE_PRESS_MS        500
#define STOP_REC_DEBOUNCE_MS   300

// ===================== STABILITY =====================
#define WDT_TIMEOUT_S        20     // any hang longer than this reboots the board automatically
#define MAX_VIDEO_SECONDS  1800     // safety stop for a forgotten recording

// ===================== CAPTURE SPEED =====================
// 1 = start the UXGA camera init right after the first button release, while we are still waiting to see
//     whether a second press (video) follows. Set to 0 to go back to the strictly sequential order.
#define SPECULATIVE_CAPTURE_INIT  0
// The sensor must have been streaming at least this long before the shot (auto exposure / white balance).
// The old code got ~300 ms from 3 discarded frames; this keeps at least as much, measured in real time.
#define CAM_SETTLE_MS  350

// ===================== AVI =====================
#define AVI_HEADER_SIZE  224        // bytes actually written by writeAviHeader()
#define AVI_W            320        // must match FRAMESIZE_QVGA used in video mode
#define AVI_H            240

// ===================== LEGACY EEPROM LAYOUT (read once for migration) =====================
#define LEGACY_EEPROM_SIZE    4
#define LEGACY_ADDR_EFFECT    0
#define LEGACY_MAGIC_ADDR     1
#define LEGACY_MAGIC_VAL      0xA5
#define LEGACY_ADDR_IDX_LO    2
#define LEGACY_ADDR_IDX_HI    3

// ===================== CAMERA MODES =====================
#define CAM_PREVIEW  0
#define CAM_CAPTURE  1
#define CAM_VIDEO    2

// ===================== TYPES =====================
enum Kind { K_PHOTO = 0, K_VIDEO = 1 };

enum Stage : uint8_t {
  ST_IDLE = 0, ST_BOOT, ST_PREVIEW,
  ST_CAP_PREP, ST_CAP_CAMERA, ST_CAP_SD, ST_CAP_WARMUP, ST_CAP_GRAB, ST_CAP_WRITE,
  ST_VID_PREP, ST_VID_SD, ST_VID_CAMERA, ST_VID_REC, ST_VID_FINISH,
  ST_WIFI_START, ST_WIFI_SERVE
};

typedef bool (*FileCb)(const String& name, void* ctx);

struct ByteBuf {                       // tiny helper to build little-endian ZIP headers
  uint8_t b[64];
  size_t  n = 0;
  void reset() { n = 0; }
  void u16(uint16_t v) { b[n++] = v & 0xFF; b[n++] = (v >> 8) & 0xFF; }
  void u32(uint32_t v) { u16(v & 0xFFFF); u16((v >> 16) & 0xFFFF); }
};

struct ZipEnt { String path; String name; uint32_t size; uint32_t crc; uint32_t off; };

// ===================== GLOBALS =====================
TFT_eSPI    tft = TFT_eSPI();
WebServer   server(HTTP_PORT);
DNSServer   dnsServer;
Preferences prefs;

bool wifiModeActive  = false;
bool grayscaleActive = false;

static uint8_t  ioBuf[8192];           // shared streaming buffer (WiFi mode only)
static uint32_t crcTable[256];
static bool     crcReady = false;

#define RTC_MAGIC 0xC11C4A55u
RTC_NOINIT_ATTR uint32_t g_rtcMagic;   // survives watchdog/software resets (not power loss)
RTC_NOINIT_ATTR uint8_t  g_rtcStage;
RTC_NOINIT_ATTR uint8_t  g_quickBoot;  // 1 = this restart follows a photo -> skip the slow boot banner
RTC_NOINIT_ATTR uint32_t g_savedIdx;   // photo number to show on the quick-boot "Saved" text
static bool               g_quick       = false;
static unsigned long      g_camReadyMs  = 0;
static unsigned long      g_pressMs     = 0;
volatile bool             g_secondPress = false;
volatile uint32_t         g_isrIgnoreUntil = 0;
static uint8_t            g_lastStage   = ST_IDLE;
static esp_reset_reason_t g_resetReason = ESP_RST_UNKNOWN;

void showMessage(const char* msg, uint16_t bgColor = TFT_BLACK, uint16_t textColor = TFT_WHITE);

// Catches a 2nd button press even while we are busy initialising the camera (blocking calls).
void IRAM_ATTR btnIsr() {
  if ((int32_t)(millis() - g_isrIgnoreUntil) >= 0) g_secondPress = true;
}

// ============================================================
//  WATCHDOG + STAGE TRACKING
// ============================================================
static void wdtInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  esp_task_wdt_config_t cfg;
  cfg.timeout_ms     = WDT_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic  = true;
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);       // true = panic -> reboot
#endif
  esp_task_wdt_add(NULL);                       // watch the Arduino loop task
}

static inline void wdtFeed() { esp_task_wdt_reset(); }

static const char* stageName(uint8_t s) {
  switch (s) {
    case ST_IDLE:       return "idle";
    case ST_BOOT:       return "boot";
    case ST_PREVIEW:    return "preview";
    case ST_CAP_PREP:   return "cap:prep";
    case ST_CAP_CAMERA: return "cap:camera";
    case ST_CAP_SD:     return "cap:sd";
    case ST_CAP_WARMUP: return "cap:warmup";
    case ST_CAP_GRAB:   return "cap:grab";
    case ST_CAP_WRITE:  return "cap:write";
    case ST_VID_PREP:   return "vid:prep";
    case ST_VID_SD:     return "vid:sd";
    case ST_VID_CAMERA: return "vid:camera";
    case ST_VID_REC:    return "vid:rec";
    case ST_VID_FINISH: return "vid:finish";
    case ST_WIFI_START: return "wifi:start";
    case ST_WIFI_SERVE: return "wifi:serve";
    default:            return "?";
  }
}

static void setStage(uint8_t s) {
  g_rtcStage = s;
  Serial.printf("[stage] %s @%lums\n", stageName(s), millis());
}

static bool isCrashReset(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
         r == ESP_RST_WDT   || r == ESP_RST_BROWNOUT;
}

static void bootDiagnostics() {
  g_resetReason = esp_reset_reason();
  if (g_resetReason == ESP_RST_POWERON || g_rtcMagic != RTC_MAGIC) {
    g_rtcMagic = RTC_MAGIC;
    g_rtcStage = ST_IDLE;
    g_quickBoot = 0;
    g_savedIdx = 0;
  }
  g_quick = (g_resetReason == ESP_RST_SW) && (g_quickBoot == 1);
  g_quickBoot = 0;
  g_lastStage = g_rtcStage;
  g_rtcStage  = ST_BOOT;
  Serial.printf("Reset reason: %d | last stage before reset: %s\n", (int)g_resetReason, stageName(g_lastStage));
}

static void waitRelease(unsigned long maxMs) {      // bounded replacement for "while (LOW);"
  unsigned long t = millis();
  while (digitalRead(BUTTON_PIN) == LOW && (millis() - t) < maxMs) {
    wdtFeed();
    delay(5);
  }
}

// ============================================================
//  NVS SETTINGS (Preferences): effect, photo counter, video counter
// ============================================================
static uint32_t nvGet(const char* key, uint32_t def) {
  prefs.begin("clicky", true);
  uint32_t v = prefs.getUInt(key, def);
  prefs.end();
  return v;
}

static void nvPut(const char* key, uint32_t v) {
  prefs.begin("clicky", false);
  prefs.putUInt(key, v);
  prefs.end();
}

static const char* kindKey(Kind k) { return k == K_PHOTO ? "pic" : "vid"; }

static uint32_t loadIndex(Kind k) {
  uint32_t v = nvGet(kindKey(k), 1);
  return v < 1 ? 1 : v;
}

static void saveIndex(Kind k, uint32_t v) {
  nvPut(kindKey(k), v);
  Serial.printf("NVS: next %s index = %u\n", kindKey(k), (unsigned)v);
}

static void saveEffect(bool grayscale) { nvPut("fx", grayscale ? 1 : 0); }

// ============================================================
//  FILE-NAME HELPERS
// ============================================================
static bool endsWithIC(const String& s, const char* ext) {
  size_t el = strlen(ext);
  if (s.length() < el) return false;
  return strcasecmp(s.c_str() + s.length() - el, ext) == 0;
}
static bool isJpegName(const String& s) { return endsWithIC(s, ".jpg") || endsWithIC(s, ".jpeg"); }
static bool isAviName(const String& s)  { return endsWithIC(s, ".avi"); }

static bool safeChars(const String& s) {
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == ' ' || c == '/')) return false;
  }
  return true;
}

// Accepts "photos/pic1.jpg", "videos/vid1.avi" and legacy "pic1.jpg" (SD root). Nothing else.
static bool isMediaPath(const String& rel) {
  if (rel.length() < 5 || rel.length() > 60) return false;
  if (!safeChars(rel)) return false;
  if (rel.indexOf("..") >= 0 || rel.indexOf("//") >= 0 || rel[0] == '/' || rel[0] == '.') return false;
  String base = rel;
  int slash = rel.indexOf('/');
  if (slash >= 0) {
    String dir = rel.substring(0, slash);
    if (dir != "photos" && dir != "videos") return false;
    base = rel.substring(slash + 1);
    if (base.indexOf('/') >= 0 || base.length() == 0 || base[0] == '.') return false;
  }
  return isJpegName(base) || isAviName(base);
}

static String baseName(const String& rel) {
  int slash = rel.lastIndexOf('/');
  return slash >= 0 ? rel.substring(slash + 1) : rel;
}

// "pic123.jpg" with prefix "pic" -> 123, otherwise 0
static uint32_t numberInName(const String& name, const char* prefix) {
  size_t pl = strlen(prefix);
  if (name.length() <= pl || strncasecmp(name.c_str(), prefix, pl) != 0) return 0;
  uint32_t v = 0;
  size_t i = pl;
  bool digits = false;
  for (; i < name.length(); i++) {
    char c = name[i];
    if (c >= '0' && c <= '9') { v = v * 10 + (uint32_t)(c - '0'); digits = true; }
    else break;
  }
  if (!digits || name[i] != '.') return 0;
  return v;
}

static String mediaPath(Kind k, uint32_t i) {
  return String(k == K_PHOTO ? "/pic" : "/vid") + String(i) + (k == K_PHOTO ? ".jpg" : ".avi");
}
static String legacyPath(Kind k, uint32_t i) {
  return String(k == K_PHOTO ? "/pic" : "/vid") + String(i) + (k == K_PHOTO ? ".jpg" : ".avi");
}
static bool mediaExists(Kind k, uint32_t i) {
  return SD_MMC.exists(mediaPath(k, i)) || SD_MMC.exists(legacyPath(k, i));
}

// ============================================================
//  FAST DIRECTORY WALK (POSIX readdir: one pass, no per-file lookup)
// ============================================================
static void forEachFile(const char* dir, FileCb cb, void* ctx) {
  String path = "/sdcard";
  if (dir[0]) { path += "/"; path += dir; }
  DIR* d = opendir(path.c_str());
  if (!d) return;
  struct dirent* de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_type == DT_DIR) continue;
    if (!cb(String(de->d_name), ctx)) break;
    wdtFeed();
  }
  closedir(d);
}

struct MaxCtx { const char* prefix; uint32_t maxv; };
static bool maxCb(const String& name, void* v) {
  MaxCtx* c = (MaxCtx*)v;
  uint32_t n = numberInName(name, c->prefix);
  if (n > c->maxv) c->maxv = n;
  return true;
}
static uint32_t scanMaxIndex(const char* dir, const char* prefix) {
  MaxCtx c = { prefix, 0 };
  forEachFile(dir, maxCb, &c);
  return c.maxv;
}

struct AnyCtx { const char* prefix; bool found; };
static bool anyCb(const String& name, void* v) {
  AnyCtx* c = (AnyCtx*)v;
  if (numberInName(name, c->prefix) > 0) { c->found = true; return false; }
  return true;
}
static bool anyMedia(Kind k) {                       // stops at the first match
  AnyCtx c = { kindKey(k), false };
  forEachFile(k == K_PHOTO ? "photos" : "videos", anyCb, &c);
  if (!c.found) forEachFile("", anyCb, &c);
  return c.found;
}

// ============================================================
//  NUMBERING
//  - the next index is a stored counter, so capture never scans the card
//  - guard: if the newest file we know about is gone AND no media of that kind exists at all,
//    the card was wiped -> restart at 1. (Deleting only the newest file does NOT reset anything.)
//  - collision loop: safety net if the counter is ever behind reality
// ============================================================
static uint32_t resolveNextIndex(Kind k) {
  uint32_t idx = loadIndex(k);
  if (idx > 1 && !mediaExists(k, idx - 1) && !anyMedia(k)) {
    Serial.printf("Index guard: no %s files left, card was wiped -> numbering restarts at 1\n", kindKey(k));
    idx = 1;
  }
  String p = mediaPath(k, idx);
  while (SD_MMC.exists(p)) {
    Serial.printf("Index collision at %u — advancing.\n", (unsigned)idx);
    idx++;
    p = mediaPath(k, idx);
    wdtFeed();
  }
  return idx;
}

// After deleting files from the gallery: walk the counter back over trailing gaps (bounded).
static void rollbackIndexAfterDelete(Kind k) {
  uint32_t orig = loadIndex(k), idx = orig;
  int steps = 0;
  while (idx > 1 && steps++ < 64 && !mediaExists(k, idx - 1)) { idx--; wdtFeed(); }
  if (idx != orig) saveIndex(k, idx);
}

static void ensureMediaDirs() {
  // Safe-root build: keep the proven storage layout used by the working firmware.
  // The gallery still lists legacy root files, so no directories are required.
}

// ============================================================
//  ONE-TIME MIGRATION (old EEPROM settings + one SD scan)
// ============================================================
static uint32_t maxU(uint32_t a, uint32_t b) { return a > b ? a : b; }

static void migrateSettingsOnce() {
  prefs.begin("clicky", false);                   // also creates the namespace on first boot
  bool done = prefs.getBool("init", false);
  prefs.end();
  if (done) return;

  Serial.println("First run of this firmware: importing old settings + scanning SD once.");

  uint32_t oldEffect = 0, oldPicIdx = 1;
  EEPROM.begin(LEGACY_EEPROM_SIZE);
  if (EEPROM.read(LEGACY_MAGIC_ADDR) == LEGACY_MAGIC_VAL) {
    oldEffect = (EEPROM.read(LEGACY_ADDR_EFFECT) == 1) ? 1 : 0;
    oldPicIdx = (uint32_t)EEPROM.read(LEGACY_ADDR_IDX_LO) | ((uint32_t)EEPROM.read(LEGACY_ADDR_IDX_HI) << 8);
  }
  EEPROM.end();

  uint32_t maxPic = 0, maxVid = 0;
  bool scanned = true;
  Serial.println("Safe-root build: skipping setup-time SD scan; capture will resolve any filename collisions.");

  prefs.begin("clicky", false);
  if (!prefs.isKey("fx")) prefs.putUInt("fx", oldEffect);
  uint32_t curPic = prefs.getUInt("pic", 1);
  uint32_t curVid = prefs.getUInt("vid", 1);
  prefs.putUInt("pic", maxU(maxU(curPic, oldPicIdx), maxPic + 1));
  prefs.putUInt("vid", maxU(curVid, maxVid + 1));
  if (scanned) prefs.putBool("init", true);
  prefs.end();

  Serial.printf("Migration done. next pic=%u next vid=%u\n", (unsigned)nvGet("pic", 1), (unsigned)nvGet("vid", 1));
}

// ============================================================
//  DISPLAY / PINS / RESTART
// ============================================================
void showMessage(const char* msg, uint16_t bgColor, uint16_t textColor) {
  tft.fillScreen(bgColor);
  tft.setTextColor(textColor, bgColor);
  tft.setTextSize(1);
  tft.drawString(msg, 10, 35);
}

static void applyEffect(bool grayscale) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  s->set_special_effect(s, grayscale ? 2 : 0);
  s->set_hmirror(s, 1);
}

// Free the pins shared between the TFT (SPI) and the SD card (SD_MMC 4-bit / 1-bit)
static void releaseSpiPins() {
  digitalWrite(TFT_CS_PIN, HIGH);
  SPI.end();
  pinMode(2,  INPUT_PULLUP);
  //pinMode(4,  INPUT_PULLUP);
  pinMode(12, INPUT_PULLUP);
  pinMode(13, INPUT_PULLUP);
  delay(50);
  gpio_reset_pin(GPIO_NUM_2);
  gpio_set_pull_mode(GPIO_NUM_2, GPIO_PULLUP_ONLY);
  delay(30);
}

// Same hand-over as releaseSpiPins(), with the settle time of your original photo path (30 ms total).
static void releaseSpiPinsFast() {
  digitalWrite(TFT_CS_PIN, HIGH);
  SPI.end();
  pinMode(2,  INPUT_PULLUP);
  //pinMode(4,  INPUT_PULLUP);
  pinMode(12, INPUT_PULLUP);
  pinMode(13, INPUT_PULLUP);
  delay(15);
  gpio_reset_pin(GPIO_NUM_2);
  gpio_set_pull_mode(GPIO_NUM_2, GPIO_PULLUP_ONLY);
  delay(15);
}

static void reinitDisplay() {
  digitalWrite(TFT_CS_PIN, HIGH);
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
}

// Clean restart: everything released first. A restart resets every peripheral to a known state,
// which is far more reliable than trying to re-initialise the display/camera/SD in place.
static void safeRestart() {
  setStage(ST_IDLE);                    // marks "this was a normal, intentional restart"
  SD_MMC.end();
  esp_camera_deinit();
  Serial.flush();
  pinMode(12, OUTPUT);                  // GPIO12 is a boot-strapping pin: leave it LOW across the reset
  digitalWrite(12, LOW);
  delay(30);
  esp_restart();
}

// Silent probe: touches only the SD pins, never the display.
static bool sdProbe() {
  releaseSpiPins();
  bool begun = SD_MMC.begin("/sdcard", true);
  uint8_t type = begun ? SD_MMC.cardType() : CARD_NONE;
  uint64_t size = (begun && type != CARD_NONE) ? SD_MMC.cardSize() : 0;
  Serial.printf("[sdcheck] begin=%d type=%d size=%llu MB\n", begun, type, size / (1024ULL * 1024ULL));
  SD_MMC.end();
  return begun && type != CARD_NONE && size > 0;
}

// Draw the message once, then poll silently. The screen keeps its image (same trick as WiFi mode).
static void showNoSdAndWait() {
  Serial.println("No SD card - waiting for insertion.");
  tft.fillScreen(TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.setTextSize(1);
  tft.drawString("NO SD CARD", 10, 15);
  tft.drawString("Insert card", 10, 32);
  tft.drawString("(FAT32)", 10, 47);

  while (true) {
    unsigned long t = millis();
    while (millis() - t < 1500) { wdtFeed(); delay(20); }
    if (sdProbe()) break;
  }
  Serial.println("SD card detected - restarting.");
  safeRestart();
}

// Restart used right after a photo is saved. No camera de-init (the reset does it), no extra delay,
// and the next boot skips the slow banner (g_quickBoot).
static void restartAfterPhoto(uint32_t savedIdx) {
  g_savedIdx  = savedIdx;
  g_quickBoot = 1;
  g_rtcStage  = ST_IDLE;
  Serial.printf("[time] button press -> restart: %lu ms\n", millis() - g_pressMs);
  SD_MMC.end();
  Serial.flush();
  pinMode(12, OUTPUT);
  digitalWrite(12, LOW);
  esp_restart();
}

static void failAndRestart(const char* msg) {
  digitalWrite(4, LOW);
  Serial.printf("FAIL: %s\n", msg);
  SD_MMC.end();
  esp_camera_deinit();
  gpio_reset_pin(GPIO_NUM_2);
  delay(20);
  reinitDisplay();
  showMessage(msg, TFT_RED, TFT_WHITE);
  delay(1500);
  safeRestart();
}

// ============================================================
//  SD CARD
// ============================================================
static bool mountSD(bool oneBit) {
  for (int i = 0; i < 3; i++) {
    wdtFeed();
    if (SD_MMC.begin("/sdcard", oneBit)) return true;
    SD_MMC.end();
    delay(120);
  }
  return false;
}


static bool mountSDForWrite() {
  if (mountSD(true)) { Serial.println("SD mounted (1-bit)."); return true; }
  return false;
}

static bool writeFileChecked(const String& path, const uint8_t* data, size_t len) {
  if (!data || len < 4096) {
    Serial.printf("Refusing suspicious write to %s (%u bytes)\n", path.c_str(), (unsigned)len);
    return false;
  }
  File f = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (!f) { Serial.println("Failed to open for writing: " + path); return false; }
  size_t w = f.write(data, len);
  f.flush();
  f.close();
  if (w != len) {
    Serial.printf("Short write %u/%u — removing %s\n", (unsigned)w, (unsigned)len, path.c_str());
    SD_MMC.remove(path);
    return false;
  }
  File check = SD_MMC.open(path.c_str(), FILE_READ);
  size_t saved = check ? check.size() : 0;
  if (check) check.close();
  if (saved != len) {
    Serial.printf("Verify failed %u/%u — removing %s\n", (unsigned)saved, (unsigned)len, path.c_str());
    SD_MMC.remove(path);
    return false;
  }
  Serial.printf("Saved: %s (%u bytes)\n", path.c_str(), (unsigned)len);
  return true;
}

// ============================================================
//  CAMERA
//  NOTE: camera_config_t is now zero-initialised. Previously its unset fields (fb_location,
//  grab_mode, sccb port ...) held whatever happened to be on the stack, which can make init
//  succeed one time and misbehave the next.
// ============================================================
static void camPowerCycle() {
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);    // power down the sensor
  delay(60);
  digitalWrite(PWDN_GPIO_NUM, LOW);     // power up again
  delay(60);
}

static bool cameraBegin(int mode) {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;

  if (mode == CAM_PREVIEW) {
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size   = FRAMESIZE_QQVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
  } else if (mode == CAM_CAPTURE) {
    config.pixel_format = PIXFORMAT_JPEG;
    if (psramFound()) {
      config.frame_size   = FRAMESIZE_UXGA;
      config.jpeg_quality = 10;
      config.fb_count     = 2;
      Serial.println("PSRAM -> UXGA (1600x1200)");
    } else {
      config.frame_size   = FRAMESIZE_SVGA;
      config.jpeg_quality = 12;
      config.fb_count     = 1;
      Serial.println("No PSRAM -> SVGA");
    }
  } else {
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_QVGA;          // 320x240 (matches AVI_W / AVI_H)
    config.jpeg_quality = 12;
    config.fb_count     = 2;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) { Serial.printf("Camera init failed (mode %d): 0x%x\n", mode, err); return false; }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_hmirror(s, 1);
    s->set_special_effect(s, grayscaleActive ? 2 : 0);
    if (mode == CAM_VIDEO) {
      s->set_gain_ctrl(s, 1);
      s->set_exposure_ctrl(s, 1);
      s->set_whitebal(s, 1);
    }
  }
  g_camReadyMs = millis();
  Serial.printf("Camera ready (mode %d)\n", mode);
  return true;
}

// up to 3 attempts; from the 2nd attempt on, the sensor is power-cycled via its PWDN pin first
static bool cameraStart(int mode) {
  for (int attempt = 0; attempt < 3; attempt++) {
    wdtFeed();
    if (attempt > 0) {
      Serial.printf("Camera init retry %d\n", attempt);
      esp_camera_deinit();
      camPowerCycle();
    }
    if (cameraBegin(mode)) return true;
  }
  return false;
}

static bool jpegLooksValid(camera_fb_t* fb) {
  if (!fb || fb->len < 4) return false;
  if (fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) return false;            // SOI
  size_t from = fb->len > 16 ? fb->len - 16 : 0;
  for (size_t i = from; i + 1 < fb->len; i++) {
    if (fb->buf[i] == 0xFF && fb->buf[i + 1] == 0xD9) return true;       // EOI
  }
  return false;
}

static void discardFrames(int count) {
  for (int i = 0; i < count; i++) {
    wdtFeed();
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(30);
  }
}

// Returns only a complete-looking JPEG frame. It never returns a known-bad frame.
static camera_fb_t* grabJpeg(int tries) {
  for (int i = 0; i < tries; i++) {
    wdtFeed();
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { delay(60); continue; }
    if (jpegLooksValid(fb)) return fb;
    Serial.println("Frame looked truncated — retrying.");
    esp_camera_fb_return(fb);
    delay(30);
  }
  return nullptr;
}

// ============================================================
//  PHOTO CAPTURE
// ============================================================
static void doCapture(bool camReady) {
  setStage(ST_CAP_PREP);
  showMessage("Capturing...", TFT_BLUE, TFT_WHITE);
  releaseSpiPinsFast();                 // TFT SPI off, SD pins free

  if (!camReady) {                      // (already done during the double-press wait when SPECULATIVE_CAPTURE_INIT = 1)
    setStage(ST_CAP_CAMERA);
    esp_camera_deinit();
    if (!cameraStart(CAM_CAPTURE)) failAndRestart("Camera init failed");
  }

  setStage(ST_CAP_SD);
  if (!mountSDForWrite()) failAndRestart("SD card error");
  ensureMediaDirs();

  // Auto exposure / white balance need the sensor to have been streaming for a while. In GRAB_LATEST mode
  // the sensor keeps running and fb_get() always returns the newest frame, so we simply make sure enough
  // real time has passed since init. When init overlapped the double-press wait this is already true.
  setStage(ST_CAP_WARMUP);
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);                 // flash ON
  while (millis() - g_camReadyMs < CAM_SETTLE_MS) { wdtFeed(); delay(5); }
  discardFrames(3);

  setStage(ST_CAP_GRAB);
  camera_fb_t* fb = grabJpeg(8);
  digitalWrite(4, LOW);
  if (!fb) failAndRestart("Frame grab failed");

  setStage(ST_CAP_WRITE);
  uint32_t idx = resolveNextIndex(K_PHOTO);
  bool ok = writeFileChecked(mediaPath(K_PHOTO, idx), fb->buf, fb->len);
  esp_camera_fb_return(fb);
  if (!ok) failAndRestart("SD write failed");
  saveIndex(K_PHOTO, idx + 1);          // pre-increment so the next shot needs no search

  restartAfterPhoto(idx);
}

// ============================================================
//  AVI + VIDEO RECORDING
// ============================================================
static void writeAviHeader(File &file, uint32_t frameCount, uint32_t totalFrameBytes,
                           uint32_t moviBytes, uint32_t usPerFrame) {
  auto w32 = [&](uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v), (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    file.write(b, 4);
  };
  auto w16 = [&](uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v), (uint8_t)(v >> 8) };
    file.write(b, 2);
  };
  auto wcc = [&](const char* cc) {
    file.write((const uint8_t*)cc, 4);
  };

  const uint32_t W = AVI_W;
  const uint32_t H = AVI_H;
  uint32_t avgFrame       = frameCount ? (totalFrameBytes / frameCount) : 15000;
  uint32_t maxBytesPerSec = (uint32_t)((uint64_t)avgFrame * 1000000ULL / (usPerFrame ? usPerFrame : 50000));

  wcc("RIFF");  w32(AVI_HEADER_SIZE - 8 + moviBytes);  wcc("AVI ");

  wcc("LIST");  w32(192);  wcc("hdrl");
  wcc("avih");  w32(56);
  w32(usPerFrame);                       // dwMicroSecPerFrame
  w32(maxBytesPerSec);
  w32(0);                                // padding granularity
  w32(0);                                // flags (no idx1 index is written)
  w32(frameCount);
  w32(0);                                // initial frames
  w32(1);                                // streams
  w32(W * H * 3);                        // suggested buffer
  w32(W); w32(H);
  w32(0); w32(0); w32(0); w32(0);

  wcc("LIST");  w32(116);  wcc("strl");
  wcc("strh");  w32(56);
  wcc("vids");  wcc("MJPG");
  w32(0);                                // flags
  w16(0); w16(0);                        // priority, language
  w32(0);                                // initial frames
  w32(usPerFrame);                       // scale  \ rate/scale = real fps
  w32(1000000);                          // rate   /
  w32(0);                                // start
  w32(frameCount);                       // length
  w32(W * H * 3);
  w32(0);                                // quality
  w32(0);                                // sample size
  w16(0); w16(0); w16(W); w16(H);

  wcc("strf");  w32(40);
  w32(40); w32(W); w32(H);
  w16(1); w16(24);
  wcc("MJPG");
  w32(W * H * 3);
  w32(0); w32(0); w32(0); w32(0);

  wcc("LIST");  w32(moviBytes + 4);  wcc("movi");
}

static bool stopButtonPressed(unsigned long recStart) {
  if (millis() - recStart <= STOP_REC_DEBOUNCE_MS) return false;
  if (digitalRead(BUTTON_PIN) != LOW) return false;
  delay(40);
  if (digitalRead(BUTTON_PIN) != LOW) return false;
  waitRelease(2000);
  return true;
}

static void recordVideo() {
  const uint32_t TARGET_FPS = 20;
  const unsigned long frameInterval = 1000 / TARGET_FPS;

  setStage(ST_VID_PREP);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("REC", 30, 22);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Single press to stop", 4, 55);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(grayscaleActive ? "Mode: GRAYSCALE" : "Mode: COLOUR", 4, 68);
  delay(1500);

  esp_camera_deinit();
  delay(50);
  releaseSpiPins();

  setStage(ST_VID_SD);
  if (!mountSDForWrite()) failAndRestart("SD card error");
  ensureMediaDirs();

  setStage(ST_VID_CAMERA);
  if (!cameraStart(CAM_VIDEO)) failAndRestart("Camera init failed");

  uint32_t idx = resolveNextIndex(K_VIDEO);
  String vidPath = mediaPath(K_VIDEO, idx);
  Serial.println("Recording to: " + vidPath);

  File vidFile = SD_MMC.open(vidPath.c_str(), FILE_WRITE);
  if (!vidFile) failAndRestart("Cannot create video");
  saveIndex(K_VIDEO, idx + 1);

  writeAviHeader(vidFile, 0, 0, 0, 1000000 / TARGET_FPS);

  for (int i = 0; i < 3; i++) {                       // warm-up frames
    wdtFeed();
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(80);
  }

  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);                              // flash ON for the whole recording

  setStage(ST_VID_REC);
  uint32_t frameCount = 0, totalFrameBytes = 0, moviBytes = 0;
  int      grabFails = 0;
  unsigned long recStart = millis();
  unsigned long lastFrameTime = recStart;
  bool stopRequested = false;
  Serial.println("Recording started.");

  while (!stopRequested) {
    wdtFeed();
    unsigned long now = millis();

    if (now - recStart > (unsigned long)MAX_VIDEO_SECONDS * 1000UL) {
      Serial.println("Max recording time reached.");
      break;
    }

    if (now - lastFrameTime < frameInterval) {
      if (stopButtonPressed(recStart)) stopRequested = true;
      delay(1);
      continue;
    }
    lastFrameTime = millis();

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Frame grab failed — skipping.");
      if (++grabFails >= 10) { Serial.println("Camera stalled — stopping."); break; }
      continue;
    }
    grabFails = 0;

    uint32_t frameLen = fb->len;
    uint8_t chunkHdr[8] = {
      '0', '0', 'd', 'c',
      (uint8_t)(frameLen), (uint8_t)(frameLen >> 8), (uint8_t)(frameLen >> 16), (uint8_t)(frameLen >> 24)
    };
    bool wrote = (vidFile.write(chunkHdr, 8) == 8) && (vidFile.write(fb->buf, frameLen) == frameLen);
    if (wrote && (frameLen & 1)) {
      uint8_t pad = 0;
      wrote = (vidFile.write(&pad, 1) == 1);
    }
    esp_camera_fb_return(fb);

    if (!wrote) {
      Serial.println("File write error — stopping.");
      break;
    }

    totalFrameBytes += frameLen;
    moviBytes       += 8 + frameLen + (frameLen & 1);
    frameCount++;

    if (stopButtonPressed(recStart)) stopRequested = true;
  }

  setStage(ST_VID_FINISH);
  digitalWrite(4, LOW);                               // flash OFF
  unsigned long elapsedMs = millis() - recStart;
  Serial.printf("Recording stopped. Frames: %u  bytes: %u  time: %lu ms\n",
                (unsigned)frameCount, (unsigned)totalFrameBytes, elapsedMs);

  if (frameCount == 0) {
    vidFile.close();
    SD_MMC.remove(vidPath);
  } else {
    uint32_t usPerFrame = (uint32_t)((uint64_t)elapsedMs * 1000ULL / frameCount);   // real average frame time
    if (usPerFrame == 0) usPerFrame = 1000000 / TARGET_FPS;
    vidFile.seek(0);
    writeAviHeader(vidFile, frameCount, totalFrameBytes, moviBytes, usPerFrame);
    vidFile.close();
  }

  Serial.println("AVI file finalised. Restarting.");
  safeRestart();
}

// ============================================================
//  WEB: HTML PAGES
// ============================================================
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
  .zip-progress{display:none;margin:-4px 0 16px 0;background:#1e1e1e;border:1px solid #2d3748;border-radius:8px;padding:10px 12px}
  .zip-progress.active{display:block}
  .zip-row{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:8px}
  .zip-label{font-size:.8rem;color:#ddd;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  .zip-percent{font-size:.8rem;color:#93c5fd;font-weight:700;min-width:42px;text-align:right}
  .zip-track{height:8px;background:#111827;border-radius:999px;overflow:hidden}
  .zip-bar{height:100%;width:0;background:#2563eb;border-radius:999px;transition:width .2s ease}
  .zip-progress.indeterminate .zip-bar{width:45%;animation:zipmove 1.1s ease-in-out infinite}
  @keyframes zipmove{0%{transform:translateX(-110%)}100%{transform:translateX(240%)}}
  .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:12px}
  .card{background:#1e1e1e;border-radius:10px;overflow:hidden;cursor:pointer;transition:outline .1s;display:block}
  .card:has(input:checked){outline:3px solid #2563eb}
  .box{position:relative;width:100%;aspect-ratio:4/3;background:#2a2a2a;overflow:hidden}
  .box img{width:100%;height:100%;object-fit:cover;display:block;opacity:0;transition:opacity .25s}
  .box img.ok{opacity:1}
  .box .ph{position:absolute;inset:0;display:none;align-items:center;justify-content:center;font-size:.7rem;color:#777;text-align:center;padding:6px}
  .card.failed .box img{display:none}
  .card.failed .box .ph{display:flex}
  .box.vid{display:flex;flex-direction:column;align-items:center;justify-content:center;background:#221a33}
  .vicon{font-size:2rem;line-height:1}
  .vtxt{font-size:.7rem;letter-spacing:.15em;color:#a78bfa;margin-top:6px}
  .card-footer{display:flex;align-items:center;gap:6px;padding:6px 8px}
  .card-footer input{width:16px;height:16px;accent-color:#2563eb;flex-shrink:0}
  .fname{font-size:.75rem;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#ccc;flex:1;min-width:0}
  .badge{font-size:.65rem;background:#7c3aed;color:#fff;border-radius:4px;padding:1px 5px;flex-shrink:0}
  .empty{color:#666;text-align:center;padding:40px;grid-column:1/-1}
  .toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%);background:#22c55e;color:#fff;
         padding:10px 20px;border-radius:10px;font-weight:600;opacity:0;transition:opacity .4s;pointer-events:none;z-index:1001;
         max-width:90vw;text-align:center}
  .toast.show{opacity:1}
  .credit{position:fixed;bottom:16px;left:16px;font-size:.75rem;color:#bbb;line-height:1.5;pointer-events:none;
          background:rgba(0,0,0,.65);padding:6px 10px;border-radius:8px;z-index:1000;backdrop-filter:blur(3px)}
  @media(prefers-color-scheme:light){
    body{background:#f3f4f6;color:#111}
    .card{background:#fff}.fname{color:#444}.empty{color:#999}
    .btn-stop{background:#e5e7eb;color:#333}
    .box{background:#e5e7eb}.box.vid{background:#ede9fe}
    .zip-progress{background:#fff;border-color:#e5e7eb}
    .zip-label{color:#333}.zip-track{background:#e5e7eb}
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
<div class="zip-progress" id="zip-progress">
  <div class="zip-row">
    <span class="zip-label" id="zip-label">Preparing ZIP...</span>
    <span class="zip-percent" id="zip-percent"></span>
  </div>
  <div class="zip-track"><div class="zip-bar" id="zip-bar"></div></div>
</div>
<div class="grid" id="grid"></div>
<div class="toast" id="toast"></div>
<div class="credit">Made by:<br>Devarshi Bohra</div>
<script>
let files=[],queue=[],active=0,paused=false,zipBusy=false;
const MAXPAR=3;                       // the board serves one request at a time; keep the queue short
const $=id=>document.getElementById(id);
const enc=encodeURIComponent;
const base=f=>f.substring(f.lastIndexOf('/')+1);
const isVideo=f=>/\.avi$/i.test(f);
const num=f=>{const m=base(f).match(/(\d+)/);return m?parseInt(m[1],10):0;};
const esc=s=>s.replace(/[&<>\x22\x27]/g,c=>'&#'+c.charCodeAt(0)+';');

const io=new IntersectionObserver(es=>{
  es.forEach(e=>{if(e.isIntersecting){io.unobserve(e.target);queue.push(e.target);}});
  pump();
},{rootMargin:'300px'});
function pump(){
  while(!paused&&active<MAXPAR&&queue.length){
    const img=queue.shift();active++;
    img.onload=()=>{img.classList.add('ok');active--;pump();};
    img.onerror=()=>{active--;if(!img.dataset.retry){img.dataset.retry='1';setTimeout(()=>{queue.push(img);pump();},1500);}else{img.closest('.card').classList.add('failed');}pump();};
    img.src=img.dataset.src;
  }
}

async function load(){
  try{
    const r=await fetch('/list',{cache:'no-store'});
    const j=await r.json();
    files=j.f;
  }catch(e){files=[];toast('Could not load file list','#ef4444');}
  files.sort((a,b)=>{const va=isVideo(a),vb=isVideo(b);if(va!==vb)return va?1:-1;return num(b)-num(a);});
  render();
}

function render(){
  queue=[];
  const photos=files.filter(f=>!isVideo(f)).length,videos=files.length-photos;
  $('sub').textContent=files.length?(photos+' photo'+(photos!==1?'s':'')+' \u00b7 '+videos+' video'+(videos!==1?'s':'')):'SD card has no photos or videos';
  const grid=$('grid');
  if(!files.length){grid.innerHTML='<p class="empty">No files found.</p>';updateToolbar();return;}
  grid.innerHTML=files.map(f=>{
    const b=esc(base(f)),v=esc(f),q=enc(f);
    if(isVideo(f)){
      return `<label class="card"><div class="box vid"><div class="vicon">&#127916;</div><div class="vtxt">VIDEO</div></div>
        <div class="card-footer"><input type="checkbox" value="${v}" onchange="updateToolbar()"><span class="badge">VID</span><span class="fname">${b}</span></div></label>`;
    }
    return `<label class="card"><div class="box"><img data-src="/download?f=${q}" alt="${b}"><span class="ph">Could not load<br>image</span></div>
      <div class="card-footer"><input type="checkbox" value="${v}" onchange="updateToolbar()"><span class="fname">${b}</span></div></label>`;
  }).join('');
  grid.querySelectorAll('img[data-src]').forEach(i=>io.observe(i));
  updateToolbar();
}

function selected(){return[...document.querySelectorAll('input[type=checkbox]:checked')].map(c=>c.value);}
function updateToolbar(){
  const s=selected();const any=s.length>0;
  $('btn-dl').disabled=!any||zipBusy;
  $('btn-del').disabled=!any||zipBusy;
  $('sel-count').textContent=any?s.length+' selected':'';
  const boxes=[...document.querySelectorAll('input[type=checkbox]')];
  const btnSel=$('btn-selall');
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
  const el=$('toast');
  el.textContent=msg;el.style.background=color;
  el.classList.add('show');setTimeout(()=>el.classList.remove('show'),3500);
}
function isIOS(){return/iPad|iPhone|iPod/.test(navigator.userAgent)&&!window.MSStream;}
function setZipProgress(show,label,pct){
  const wrap=$('zip-progress'),bar=$('zip-bar'),txt=$('zip-label'),num=$('zip-percent');
  if(!wrap)return;
  wrap.classList.toggle('active',!!show);
  wrap.classList.toggle('indeterminate',show&&pct==null);
  if(label)txt.textContent=label;
  if(pct==null){
    num.textContent='';
    bar.style.width='0%';
  }else{
    const p=Math.max(0,Math.min(100,Math.round(pct)));
    num.textContent=p+'%';
    bar.style.width=p+'%';
  }
}
function triggerBlobDownload(blob,name){
  const url=URL.createObjectURL(blob);
  const a=document.createElement('a');a.href=url;a.download=name;
  document.body.appendChild(a);a.click();a.remove();
  setTimeout(()=>URL.revokeObjectURL(url),2000);
}

async function downloadSelected(){
  const sel=selected();if(!sel.length)return;
  if(zipBusy)return;
  if(sel.length===1){                               // one file: plain download
    const url='/download?f='+enc(sel[0]);
    if(isIOS()){window.open(url,'_blank');}
    else{const a=document.createElement('a');a.href=url;a.download=base(sel[0]);document.body.appendChild(a);a.click();a.remove();}
    return;
  }
  
  zipBusy=true;updateToolbar();
  paused=true;queue=[];                             // stop image requests while the ZIP streams
  setZipProgress(true,'Preparing ZIP...',null);
  toast('Preparing '+sel.length+' files as one ZIP. Keep this page open.','#2563eb');
  try{
    const body=new URLSearchParams();sel.forEach(f=>body.append('files',f));
    const r=await fetch('/zip',{method:'POST',body});
    if(!r.ok)throw new Error('ZIP failed');
    const ct=(r.headers.get('content-type')||'').toLowerCase();
    if(!ct.includes('zip'))throw new Error('Unexpected response');
    const total=parseInt(r.headers.get('content-length')||'0',10);
    let blob;
    if(r.body&&r.body.getReader){
      const reader=r.body.getReader(),chunks=[];let received=0;
      while(true){
        const {done,value}=await reader.read();
        if(done)break;
        chunks.push(value);received+=value.length;
        if(total>0){
          const pct=Math.min(99,Math.round(received*100/total));
          setZipProgress(true,'Downloading ZIP...',pct);
        }else{
          setZipProgress(true,'Downloading ZIP...',null);
        }
      }
      setZipProgress(true,'ZIP ready',100);
      blob=new Blob(chunks,{type:'application/zip'});
    }else{
      setZipProgress(true,'Downloading ZIP...',null);
      blob=await r.blob();
      setZipProgress(true,'ZIP ready',100);
    }
    triggerBlobDownload(blob,'clicky.zip');
    toast('ZIP ready');
  }catch(e){
    setZipProgress(true,'ZIP download failed',0);
    toast('ZIP download failed; try selecting fewer files','#ef4444');
  }finally{
    setTimeout(()=>setZipProgress(false,'',0),1800);
    zipBusy=false;updateToolbar();
    paused=false;
  }
}

async function deleteSelected(){
  const sel=selected();if(!sel.length)return;
  if(!confirm('Delete '+sel.length+' file(s)?'))return;
  const body=new URLSearchParams();sel.forEach(f=>body.append('files',f));
  try{
    const r=await fetch('/delete',{method:'POST',body});
    const j=await r.json();toast('Deleted '+j.deleted+' file(s)');
  }catch(e){toast('Delete failed','#ef4444');}
  await load();
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

// ============================================================
//  WEB: STREAMING HELPERS
// ============================================================
static void crcInit() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    crcTable[i] = c;
  }
  crcReady = true;
}

static uint32_t crcUpdate(uint32_t crc, const uint8_t* d, size_t n) {
  crc = ~crc;
  while (n--) crc = crcTable[(crc ^ *d++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// Write everything or fail. Gives up if the client makes no progress for 8 s.
static bool clientWriteAll(WiFiClient& c, const uint8_t* d, size_t n) {
  unsigned long lastProgress = millis();
  while (n) {
    if (!c.connected()) return false;
    size_t w = c.write(d, n);
    if (w > 0) { d += w; n -= w; lastProgress = millis(); }
    else {
      if (millis() - lastProgress > 8000) return false;
      delay(2);
    }
    wdtFeed();
  }
  return true;
}

// Sends one SD file with a known Content-Length using the large shared buffer.
static void streamFileOut(File& f, const char* mime, const char* cacheCtl) {
  size_t size = f.size();
  server.sendHeader("Cache-Control", cacheCtl);
  server.setContentLength(size);
  server.send(200, mime, "");
  WiFiClient& c = server.client();
  c.setNoDelay(true);
  size_t left = size;
  while (left > 0) {
    wdtFeed();
    size_t want = left < sizeof(ioBuf) ? left : sizeof(ioBuf);
    size_t n = f.read(ioBuf, want);
    if (n == 0) break;
    if (!clientWriteAll(c, ioBuf, n)) { c.stop(); return; }
    left -= n;
  }
  if (left > 0) c.stop();               // short read -> make the browser see a failed download
}

// ============================================================
//  WEB: HANDLERS
// ============================================================
static void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", HTML_PAGE);
}

static void handleCaptivePortal() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

static void handleNotFound() {
  String host = server.hostHeader();
  if (host == "captive.apple.com" || host == "www.apple.com" || host == "apple.com") {
    server.send(200, "text/html",
      "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    return;
  }
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

struct ListCtx { String* chunk; const char* dir; bool* any; };
static bool listCb(const String& name, void* v) {
  ListCtx* c = (ListCtx*)v;
  if (name.length() == 0 || name[0] == '.') return true;
  if (!(isJpegName(name) || isAviName(name))) return true;
  if (!safeChars(name)) return true;
  if (*c->any) *c->chunk += ',';
  *c->any = true;
  *c->chunk += '"';
  if (c->dir[0]) { *c->chunk += c->dir; *c->chunk += '/'; }
  *c->chunk += name;
  *c->chunk += '"';
  if (c->chunk->length() > 1000) { server.sendContent(*c->chunk); *c->chunk = ""; }
  return true;
}

// {"f":["photos/pic1.jpg","videos/vid1.avi",...]}  — streamed, so no giant String
static void handleList() {
  server.sendHeader("Cache-Control", "no-cache");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  String chunk;
  chunk.reserve(1200);
  chunk = "{\"f\":[";
  bool any = false;

  ListCtx a = { &chunk, "photos", &any };  forEachFile("photos", listCb, &a);
  ListCtx b = { &chunk, "videos", &any };  forEachFile("videos", listCb, &b);
  ListCtx r = { &chunk, "",       &any };  forEachFile("",       listCb, &r);   // legacy files in the SD root

  chunk += "]}";
  server.sendContent(chunk);
  server.sendContent("");
}

static void handleDownload() {
  String rel = server.arg("f");
  if (!isMediaPath(rel)) { server.send(400, "text/plain", "Bad request"); return; }
  File file = SD_MMC.open("/" + rel);
  if (!file || file.isDirectory()) { server.send(404, "text/plain", "Not found"); return; }

  String name = baseName(rel);
  const char* mime = isAviName(rel) ? "video/x-msvideo" : "image/jpeg";

  String disposition = "attachment";
  if (server.hasArg("inline")) {
    disposition = "inline";
  } else if (server.hasHeader("User-Agent")) {
    String ua = server.header("User-Agent");
    if (ua.indexOf("iPhone") >= 0 || ua.indexOf("iPad") >= 0 || ua.indexOf("iPod") >= 0) disposition = "inline";
  }
  server.sendHeader("Content-Disposition", disposition + "; filename=\"" + name + "\"");
  streamFileOut(file, mime, "no-cache");
  file.close();
}

// POST /zip  files=<path>&files=<path>...  ->  one stored (uncompressed) ZIP, streamed from the SD card.
// CRCs and sizes are precomputed so phones see a simple, widely-compatible ZIP.
static void handleZip() {
  if (!crcReady) crcInit();

  std::vector<ZipEnt> ents;
  uint64_t total = 22;                                       // end-of-central-directory record
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) != "files") continue;
    String rel = server.arg(i);
    if (!isMediaPath(rel)) continue;
    File f = SD_MMC.open("/" + rel);
    if (!f || f.isDirectory()) { if (f) f.close(); continue; }
    ZipEnt e;
    e.path = "/" + rel;
    e.name = baseName(rel);
    e.size = (uint32_t)f.size();
    e.crc  = 0;
    e.off  = 0;
    uint32_t done = 0;
    while (done < e.size) {
      wdtFeed();
      size_t want = e.size - done;
      if (want > sizeof(ioBuf)) want = sizeof(ioBuf);
      size_t n = f.read(ioBuf, want);
      if (n == 0) break;
      e.crc = crcUpdate(e.crc, ioBuf, n);
      done += n;
    }
    f.close();
    if (done != e.size) continue;
    ents.push_back(e);
    total += 30 + (uint64_t)e.name.length() + e.size + 46 + (uint64_t)e.name.length();
    wdtFeed();
  }
  if (ents.empty()) { server.send(404, "text/plain", "No files"); return; }
  if (total > 0xFFFFFFFFULL) { server.send(413, "text/plain", "Selection is over 4 GB - download it in smaller parts"); return; }

  server.sendHeader("Content-Disposition", "attachment; filename=\"clicky.zip\"");
  server.sendHeader("Cache-Control", "no-cache");
  server.setContentLength((size_t)total);
  server.send(200, "application/zip", "");

  WiFiClient& c = server.client();
  c.setNoDelay(true);
  ByteBuf h;
  uint32_t pos = 0;
  bool ok = true;

  // --- local file headers + data ---
  for (size_t i = 0; i < ents.size() && ok; i++) {
    ZipEnt& e = ents[i];
    File f = SD_MMC.open(e.path);
    if (!f) { ok = false; break; }

    e.off = pos;
    h.reset();
    h.u32(0x04034b50);                 // local file header signature
    h.u16(20);                         // version needed
    h.u16(0);                          // flags
    h.u16(0);                          // method: stored
    h.u16(0); h.u16(0x21);             // time, date (1980-01-01)
    h.u32(e.crc); h.u32(e.size); h.u32(e.size);
    h.u16(e.name.length()); h.u16(0);  // name length, extra length
    ok = clientWriteAll(c, h.b, h.n) &&
         clientWriteAll(c, (const uint8_t*)e.name.c_str(), e.name.length());
    pos += h.n + e.name.length();

    uint32_t sent = 0;
    while (ok && sent < e.size) {
      wdtFeed();
      size_t want = e.size - sent;
      if (want > sizeof(ioBuf)) want = sizeof(ioBuf);
      size_t n = f.read(ioBuf, want);
      if (n == 0) { ok = false; break; }
      ok = clientWriteAll(c, ioBuf, n);
      sent += n;
      pos  += n;
    }
    f.close();
    if (!ok) break;
  }

  // --- central directory + end record ---
  if (ok) {
    uint32_t cdStart = pos;
    for (size_t i = 0; i < ents.size() && ok; i++) {
      ZipEnt& e = ents[i];
      h.reset();
      h.u32(0x02014b50);
      h.u16(20); h.u16(20);            // version made by / needed
      h.u16(0); h.u16(0);              // flags, method
      h.u16(0); h.u16(0x21);           // time, date
      h.u32(e.crc); h.u32(e.size); h.u32(e.size);
      h.u16(e.name.length());
      h.u16(0); h.u16(0);              // extra, comment
      h.u16(0); h.u16(0);              // disk, internal attrs
      h.u32(0);                        // external attrs
      h.u32(e.off);
      ok = clientWriteAll(c, h.b, h.n) &&
           clientWriteAll(c, (const uint8_t*)e.name.c_str(), e.name.length());
      pos += h.n + e.name.length();
    }
    if (ok) {
      h.reset();
      h.u32(0x06054b50);
      h.u16(0); h.u16(0);
      h.u16(ents.size()); h.u16(ents.size());
      h.u32(pos - cdStart); h.u32(cdStart);
      h.u16(0);
      ok = clientWriteAll(c, h.b, h.n);
    }
  }

  if (!ok) {
    Serial.println("ZIP aborted (client left or SD read error).");
    c.stop();
  }
}

struct NameCtx { std::vector<String>* v; size_t maxN; };
static bool collectCb(const String& name, void* x) {
  NameCtx* c = (NameCtx*)x;
  if (name.length() && name[0] != '.' && (isJpegName(name) || isAviName(name))) c->v->push_back(name);
  return c->v->size() < c->maxN;
}

// Remove every jpg/avi in a directory, in small batches (never deletes while iterating).
static int purgeDir(const char* dir) {
  int deleted = 0;
  while (true) {
    std::vector<String> names;
    NameCtx ctx = { &names, 24 };
    forEachFile(dir, collectCb, &ctx);
    if (names.empty()) break;
    int removedNow = 0;
    for (size_t i = 0; i < names.size(); i++) {
      String p = "/";
      if (dir[0]) { p += dir; p += "/"; }
      p += names[i];
      if (SD_MMC.remove(p)) { deleted++; removedNow++; }
      wdtFeed();
    }
    if (removedNow == 0) break;
  }
  return deleted;
}

static void handleDelete() {
  int deleted = 0;
  bool photoDel = false, vidDel = false;
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) != "files") continue;
    String rel = server.arg(i);
    if (!isMediaPath(rel)) continue;
    if (SD_MMC.remove("/" + rel)) {
      deleted++;
      Serial.println("Deleted: /" + rel);
      if (isJpegName(rel)) {
        photoDel = true;
      } else {
        vidDel = true;
      }
    }
    wdtFeed();
  }
  if (photoDel) rollbackIndexAfterDelete(K_PHOTO);
  if (vidDel)   rollbackIndexAfterDelete(K_VIDEO);
  server.send(200, "application/json", "{\"deleted\":" + String(deleted) + "}");
}

static void handleDeleteAll() {
  int deleted = purgeDir("photos") + purgeDir("videos") + purgeDir("") ;
  saveIndex(K_PHOTO, 1);
  saveIndex(K_VIDEO, 1);
  server.send(200, "application/json", "{\"deleted\":" + String(deleted) + "}");
}

static void stopWifiMode() {
  Serial.println("Stopping WiFi mode...");
  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  SD_MMC.end();
  wifiModeActive = false;
  delay(500);
  safeRestart();
}

static void handleStop() {
  server.send(200, "text/plain", "Restarting...");
  delay(300);
  stopWifiMode();
}

static void handleOTAPage() {
  if (!server.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
    return server.requestAuthentication();
  }
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", OTA_PAGE);
}

static void handleOTAUpload() {
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
        safeRestart();
      }
    },
    []() {
      if (!server.authenticate(OTA_USERNAME, OTA_PASSWORD)) return;
      wdtFeed();
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

// ============================================================
//  WIFI MODE
// ============================================================
static void startWifiMode() {
  setStage(ST_WIFI_START);
  Serial.println("Entering WiFi mode...");

  esp_camera_deinit();
  delay(100);

  WiFi.persistent(false);                              // don't write WiFi settings to flash every time
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);                                // power-save off: much steadier throughput / latency
  const char* pass = (strlen(AP_PASSWORD) >= 8) ? AP_PASSWORD : nullptr;
  bool apOk = WiFi.softAP(AP_SSID, pass, AP_CHANNEL, 0, AP_MAX_CLIENTS);
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

  // The display stays as drawn; free its pins so the SD card can use them (1-bit is plenty for a gallery).
  releaseSpiPins();
  if (mountSD(true)) {
    Serial.println("SD mounted OK (1-bit, WiFi mode).");
    ensureMediaDirs();
  } else {
    Serial.println("SD mount failed — gallery will be empty.");
  }

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  server.on("/hotspot-detect.html",       HTTP_GET, handleCaptivePortal);
  server.on("/library/test/success.html", HTTP_GET, handleCaptivePortal);
  server.on("/generate_204",              HTTP_GET, handleCaptivePortal);
  server.on("/connecttest.txt",           HTTP_GET, handleCaptivePortal);
  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/list",      HTTP_GET,  handleList);
  server.on("/download",  HTTP_GET,  handleDownload);
  server.on("/zip",       HTTP_POST, handleZip);
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
  setStage(ST_WIFI_SERVE);
}

// ============================================================
//  BUTTON HANDLING
// ============================================================
static void handleButton() {
  unsigned long pressStart = millis();
  g_pressMs = pressStart;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString("Capture mode", 4, 20);
  tft.drawRect(4, 50, 152, 10, TFT_DARKGREY);

  int lastPhase = 0;

  while (digitalRead(BUTTON_PIN) == LOW) {
    wdtFeed();
    unsigned long held = millis() - pressStart;

    int phase;
    uint16_t barColor;
    if (held < TOGGLE_HOLD_MS)      { phase = 0; barColor = TFT_YELLOW; }
    else if (held < WIFI_HOLD_MS)   { phase = 1; barColor = TFT_CYAN;   }
    else                            { phase = 2; barColor = TFT_GREEN;  }

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

    int barWidth = (int)map(min(held, (unsigned long)WIFI_HOLD_MS), 0, WIFI_HOLD_MS, 0, 148);
    tft.fillRect(5, 51, 150, 8, TFT_BLACK);
    tft.fillRect(5, 51, barWidth, 8, barColor);

    if (held >= WIFI_HOLD_MS) {
      waitRelease(3000);
      delay(100);
      startWifiMode();
      return;
    }
    delay(30);
  }

  unsigned long held = millis() - pressStart;

  if (held >= TOGGLE_HOLD_MS) {
    // --- toggle grayscale ---
    grayscaleActive = !grayscaleActive;
    saveEffect(grayscaleActive);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Effect toggled!", 4, 20);
    tft.setTextColor(grayscaleActive ? TFT_LIGHTGREY : TFT_GREEN, TFT_BLACK);
    tft.drawString(grayscaleActive ? "GRAYSCALE" : "NORMAL", 4, 35);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Restarting...", 4, 50);
    delay(1200);
    safeRestart();
    return;
  }

  // --- short press: single (photo) or first of a double (video) ---
  unsigned long releaseTime = millis();
  bool camReady = false;

#if SPECULATIVE_CAPTURE_INIT
  // The wait below is pure dead time, and the UXGA camera init is the slowest step that does not touch the
  // display/SD pins. So start it now. The TFT keeps showing the last frame; a 2nd press is caught by the
  // interrupt even though init blocks for a few hundred ms. If it was a double press, the capture camera is
  // simply de-initialised again by the video path (video start is ~0.4 s slower, photos ~0.4 s faster).
  g_secondPress = false;
  g_isrIgnoreUntil = millis() + 60;                     // ignore contact bounce of the release we just saw
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), btnIsr, FALLING);
  esp_camera_deinit();
  setStage(ST_CAP_CAMERA);
  camReady = cameraStart(CAM_CAPTURE);
#endif

  bool secondPress = false;
  while (millis() - releaseTime < DOUBLE_PRESS_MS) {
    wdtFeed();
    bool pressed = g_secondPress;
    if (!pressed && digitalRead(BUTTON_PIN) == LOW) {
      delay(40);
      pressed = (digitalRead(BUTTON_PIN) == LOW);
    }
    if (pressed) {
      secondPress = true;
      waitRelease(1000);
      delay(50);
      break;
    }
    delay(5);
  }

#if SPECULATIVE_CAPTURE_INIT
  detachInterrupt(digitalPinToInterrupt(BUTTON_PIN));
#endif

  if (secondPress) {
    Serial.println("Double press — starting video recording");
    esp_camera_deinit();
    delay(50);
    tft.fillScreen(TFT_BLACK);
    recordVideo();                      // never returns (ends with a restart)
  } else {
    Serial.println("Single press — capturing photo");
    doCapture(camReady);                // never returns (ends with a restart)
  }
}

static void showPreviewFrame() {
  static uint8_t fails = 0;
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    if (++fails == 30) {
      Serial.println("Preview frame missing; keeping controls alive.");
      showMessage("Preview waiting", TFT_ORANGE, TFT_BLACK);
    }
    return;
  }
  fails = 0;

  tft.startWrite();
  tft.setSwapBytes(false);
  tft.pushImage(0, 0, fb->width, 80, (uint16_t*)fb->buf + 20 * 160);
  tft.endWrite();

  esp_camera_fb_return(fb);
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== CLICKY: Preview + Capture + Video + WiFi ===");

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);     // kept as before; the watchdog now catches the hangs this can cause

  bootDiagnostics();
  wdtInit();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(TFT_CS_PIN, OUTPUT);
  digitalWrite(TFT_CS_PIN, HIGH);

  bool sdOk = true;
  if (!g_quick) sdOk = sdProbe();      // BEFORE the display is initialised

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  if (g_quick) {
    // Restart that follows a photo: no banner, just a quick confirmation while the preview camera starts.
    char buf[24];
    snprintf(buf, sizeof(buf), "Saved pic%u", (unsigned)g_savedIdx);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(buf, 10, 35);
  } else {
    showMessage("Initializing...");
  }

  // If the last run froze and the watchdog rebooted us, say where it froze.
  if (isCrashReset(g_resetReason) && g_lastStage != ST_IDLE && g_lastStage != ST_BOOT) {
    tft.fillScreen(TFT_MAROON);
    tft.setTextColor(TFT_WHITE, TFT_MAROON);
    tft.drawString("Recovered after hang", 6, 25);
    tft.setTextColor(TFT_YELLOW, TFT_MAROON);
    tft.drawString(stageName(g_lastStage), 6, 42);
    delay(1800);
    showMessage("Initializing...");
  }

  if (!sdOk) showNoSdAndWait();        // never returns until a card is found (then restarts)
  migrateSettingsOnce();                         // first boot only: EEPROM -> NVS + one SD scan
  grayscaleActive = (nvGet("fx", 0) == 1);
  Serial.printf("Effect: %s | next photo %u | next video %u\n",
                grayscaleActive ? "GRAYSCALE" : "NORMAL",
                (unsigned)loadIndex(K_PHOTO), (unsigned)loadIndex(K_VIDEO));

  setStage(ST_PREVIEW);
  if (!cameraStart(CAM_PREVIEW)) {
    showMessage("Camera FAILED!", TFT_RED, TFT_WHITE);
    delay(3000);
    safeRestart();
  }
  applyEffect(grayscaleActive);

  tft.fillScreen(TFT_BLACK);
  if (!g_quick) {                                // the effect banner only on a normal start
    tft.setTextColor(grayscaleActive ? TFT_LIGHTGREY : TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    tft.drawString(grayscaleActive ? "Effect: GRAYSCALE" : "Effect: NORMAL", 6, 35);
    delay(900);
    tft.fillScreen(TFT_BLACK);
  }
  Serial.printf("[boot] preview ready @%lums (%s start)\n", millis(), g_quick ? "quick" : "normal");

  Serial.println("Ready. Short=capture | Double=video | 3-5s=toggle | 5s+=WiFi");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  wdtFeed();

  if (wifiModeActive) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(1);
    return;
  }

  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      handleButton();
      if (wifiModeActive) return;                // camera is off in WiFi mode
    }
  }

  showPreviewFrame();
}
