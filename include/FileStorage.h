#pragma once
#include <FS.h>
#include "SD_MMC.h"
#include <vector>

struct WiFiCred {
    String ssid;
    String password;
};

extern std::vector<WiFiCred> wifiList;

namespace FileStorage {
    bool initSD();
    void showAllFiles();
    void loadWiFiFromSD();
    void saveWiFiToSD(String ssid, String password);
    void deleteWiFiFromSD(String ssid);
    File getFile(String path);
}
