#pragma once
#include <Adafruit_NeoPixel.h>

namespace LEDControl {
  void init();
  void setColor(uint8_t r, uint8_t g, uint8_t b);
}
