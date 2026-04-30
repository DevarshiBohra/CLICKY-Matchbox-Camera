#pragma once
#include "esp_camera.h"
#include <Arduino.h>

void captureAndSave();
void saveImage(String path, camera_fb_t* fb);