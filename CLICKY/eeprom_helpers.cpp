#include "eeprom_helpers.h"
#include "config.h"
#include <EEPROM.h>

extern bool grayscaleActive;

void loadEffectFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t magic = EEPROM.read(EEPROM_MAGIC_ADDR);
  if (magic != EEPROM_MAGIC_VAL) {
    EEPROM.write(EEPROM_ADDR_EFFECT, 0);
    EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
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