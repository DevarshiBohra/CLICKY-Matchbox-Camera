#pragma once
#include <TFT_eSPI.h>

extern TFT_eSPI tft;

void showMessage(const char* msg, uint16_t bgColor = TFT_BLACK, uint16_t textColor = TFT_WHITE);