#include "sd_capture.h"
#include "SD_MMC.h"
#include <Arduino.h>

void captureAndSave() {
  Serial.printf("GPIO2 level before SD mount: %d\n", digitalRead(2));

  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD Card mount failed!");
    return;
  }
  Serial.println("SD mounted.");

  for (int i = 0; i < 2; i++) {
    camera_fb_t* discard = esp_camera_fb_get();
    if (discard) esp_camera_fb_return(discard);
    delay(150);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Frame capture failed!");
    SD_MMC.end();
    return;
  }

  int index = 1;
  while (true) {
    String path = "/pic" + String(index) + ".jpg";
    if (!SD_MMC.exists(path)) {
      saveImage(path, fb);
      break;
    }
    index++;
  }

  esp_camera_fb_return(fb);
  SD_MMC.end();
  Serial.println("SD unmounted. Resetting...");
  esp_restart();
}

void saveImage(String path, camera_fb_t* fb) {
  File file = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }
  file.write(fb->buf, fb->len);
  file.close();
  Serial.println("Saved: " + path + " (" + String(fb->len) + " bytes)");
}