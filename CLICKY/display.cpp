#include "display.h"

void showMessage(const char* msg, uint16_t bgColor, uint16_t textColor) {
  tft.fillScreen(bgColor);
  tft.setTextColor(textColor, bgColor);
  tft.setTextSize(1);
  tft.drawString(msg, 10, 35);
}