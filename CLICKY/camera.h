#pragma once
#include "esp_camera.h"

bool initCameraForPreview();
bool initCameraForCapture();
void applyEffect(bool grayscale);