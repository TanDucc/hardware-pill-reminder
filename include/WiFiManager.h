#pragma once
#include <WiFi.h>
#include "FileStorage.h"
#include "LEDControl.h"
#include <cstring>
#include <WiFiClientSecure.h>

namespace WiFiManager {
  int findBestWiFi();
  bool connectBestWiFi();
  void monitorConnection();
}
