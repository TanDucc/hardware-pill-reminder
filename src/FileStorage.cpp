#include "FileStorage.h"

// ---------------- Pin cấu hình ----------------
#define SD_CLK 39
#define SD_CMD 38
#define SD_D0  40

std::vector<WiFiCred> wifiList;

// ---------------- Namespace ----------------
namespace FileStorage {

    // --- Khởi động SD card ---
    bool initSD() {
        if(!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0)) {
            Serial.println("FileStorage log: ❌ SD_MMC: đổi chân thất bại!");
            return false;
        }

        if (!SD_MMC.begin("/sdcard", true, false, 10)) {
            Serial.println("FileStorage log: ❌ SD_MMC Mount Failed!");
            return false;
        }

        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        Serial.printf("FileStorage log: ✅ SD_MMC Mounted! Dung lượng: %llu MB", cardSize);
        return true;
    }

    // --- Hiển thị tất cả file trong SD card ---
    void showAllFile() {
        File root = SD_MMC.open("/");
        if (!root) {
            Serial.println("FileStorage log: ❌ Failed to open root directory!");
            return;
        }

        Serial.println("FileStorage log: 📂 File list in SD:");
        File f = root.openNextFile();
        while (f) {
            Serial.printf("   %s (%u bytes)", f.name(), (unsigned int)f.size());
            f = root.openNextFile();
        }
    }

    // --- Đọc danh sách WiFi ---
    void loadWiFiFromSD() {
        wifiList.clear();

        if (!SD_MMC.exists("/wifi/wifi.txt")) {
            Serial.println("FileStorage log: ⚠️ Không tìm thấy wifi.txt");
            return;
        }

        File file = SD_MMC.open("/wifi/wifi.txt", "r");
        if (!file) {
            Serial.println("FileStorage log: ❌ Không mở được wifi.txt");
            return;
        }

        while (file.available()) {
            String line = file.readStringUntil('\n');
            line.trim();
            if (line.isEmpty()) continue;

            int sep = line.indexOf('|');
            if (sep > 0) {
                WiFiCred cred;
                cred.ssid = line.substring(0, sep);
                cred.password = line.substring(sep + 1);
                wifiList.push_back(cred);
                Serial.printf("🔍 WiFi: %s\n", cred.ssid.c_str());
            }
        }

        file.close();
        Serial.printf("FileStorage log: 📶 Đã nạp %d WiFi từ file\n", wifiList.size());
    }

    // --- Lưu WiFi ---
    void saveWiFiToSD(String ssid, String password) {
        File file = SD_MMC.open("/wifi/wifi.txt", "a");
        if (!file) {
            Serial.println("FileStorage log: ❌ Không thể ghi file wifi.txt");
            return;
        }
        file.println(ssid + "|" + password);
        file.close();
        Serial.printf("FileStorage log: 💾 Đã lưu WiFi: %s", ssid.c_str());
    }

    // --- Xoá WiFi ---
    void deleteWiFiFromSD(String ssid) {
        if (!SD_MMC.exists("/wifi/wifi.txt")) {
            Serial.println("FileStorage log: ⚠️ Không tìm thấy wifi.txt");
            return;
        }

        File file = SD_MMC.open("/wifi/wifi.txt", "r");
        if (!file) {
            Serial.println("FileStorage log: ❌ Không mở được wifi.txt");
            return;
        }

        std::vector<WiFiCred> newList;
        bool found = false;

        while (file.available()) {
            String line = file.readStringUntil('\n');
            line.trim();
            if (line.isEmpty()) continue;

            int sep = line.indexOf('|');
            if (sep > 0) {
                String existingSsid = line.substring(0, sep);
                String existingPassword = line.substring(sep + 1);
                if (existingSsid != ssid) {
                    WiFiCred cred;
                    cred.ssid = existingSsid;
                    cred.password = existingPassword;
                    newList.push_back(cred);
                } else {
                    found = true;
                }
            }
        }

        file.close();

        if (!found) {
            Serial.printf("FileStorage log: ⚠️ Không tìm thấy WiFi: %s để xoá\n", ssid.c_str());
            return;
        }

        // Ghi lại file với danh sách mới
        file = SD_MMC.open("/wifi/wifi.txt", "w");
        if (!file) {
            Serial.println("FileStorage log: ❌ Không thể ghi file wifi.txt");
            return;
        }

        for (const auto& cred : newList) {
            file.println(cred.ssid + "|" + cred.password);
        }

        file.close();
        Serial.printf("FileStorage log: 🗑️ Đã xoá WiFi: %s\n", ssid.c_str());
    }

}
