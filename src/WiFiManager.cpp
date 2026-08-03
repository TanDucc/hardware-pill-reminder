#include "WiFiManager.h"

namespace WiFiManager {
    // Lưu thông tin mạng cuối cùng
    String lastSSID = "";
    String lastPASS = "";
    uint8_t lastBSSID[6] = {0};
    int lastChannel = 0;
    unsigned long lastReconnectAttempt = 0;

    // Thiết lập chu kỳ kiểm tra lại WiFi (ms)
    const unsigned long RECONNECT_INTERVAL = 10000;

    int findBestWiFi() {
        int n = WiFi.scanNetworks();
        if (n <= 0) return -1;

        int bestIndex = -1;
        int bestRSSI = -999;

        for (int i = 0; i < n; i++) {
            String foundSSID = WiFi.SSID(i);
            int rssi = WiFi.RSSI(i);

            for (size_t j = 0; j < wifiList.size(); j++) {
                if (wifiList[j].ssid == foundSSID) {
                    if (rssi > bestRSSI) {
                        bestRSSI = rssi;
                        bestIndex = j;
                        memcpy(lastBSSID, WiFi.BSSID(i), 6);
                        lastChannel = WiFi.channel(i);
                    }
                }
            }
        }
        return bestIndex;
    }

    bool connectBestWiFi() {
        WiFi.mode(WIFI_STA);
        // WiFi.setSleep(WIFI_PS_MIN_MODEM); // Giữ WiFi trong light sleep

        // ⚡ Nếu có thông tin mạng trước đó → reconnect nhanh
        if (lastSSID.length() > 0) {
            Serial.printf("WiFiManager log: ⚡ Thử reconnect nhanh tới %s...\n", lastSSID.c_str());
            WiFi.begin(lastSSID.c_str(), lastPASS.c_str(), lastChannel, lastBSSID, true);

            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
                delay(200);
            }

            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("WiFiManager log: ✅ Reconnect nhanh thành công tới %s\n", lastSSID.c_str());
                LEDControl::setColor(0, 255, 0);
                return true;
            }
            Serial.println("WiFiManager log: ❌ Reconnect nhanh thất bại, quét lại mạng...");
        }

        int best = findBestWiFi();
        if (best < 0) {
            Serial.println("WiFiManager log: ❌ Không tìm thấy WiFi phù hợp trong danh sách!");
            return false;
        }

        for (int attempt = 1; attempt <= 3; ++attempt) {
            Serial.printf("WiFiManager log: 🔗 [Lần %d] Kết nối tới %s...\n", attempt, wifiList[best].ssid.c_str());
            WiFi.begin(wifiList[best].ssid.c_str(), wifiList[best].password.c_str());

            unsigned long start = millis();
            LEDControl::setColor(255, 165, 0); // Màu cam: đang kết nối

            while (WiFi.status() != WL_CONNECTED && millis() - start < 7000) {
                delay(200);
            }

            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("WiFiManager log: ✅ Kết nối thành công tới %s\n", wifiList[best].ssid.c_str());
                LEDControl::setColor(0, 255, 0);

                lastSSID = wifiList[best].ssid;
                lastPASS = wifiList[best].password;
                return true;
            } else {
                Serial.printf("WiFiManager log: ❌ Thất bại lần %d\n", attempt);
                LEDControl::setColor(255, 0, 0);
                delay(500);
            }
        }
        Serial.println("WiFiManager log: ❌ Không thể kết nối sau nhiều lần thử!");
        return false;
    }

    void monitorConnection() {
        // Kiểm tra WiFi mỗi 10s để tránh spam
        if (millis() - lastReconnectAttempt < RECONNECT_INTERVAL) return;
        lastReconnectAttempt = millis();

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFiManager log: ⚠️ Mất kết nối WiFi, đang thử reconnect...");
            LEDControl::setColor(255, 0, 0);  // Đỏ khi mất kết nối

            // ⚙️ Thử reconnect nhanh trước
            WiFi.disconnect(false); // không xóa thông tin SSID
            WiFi.begin(lastSSID.c_str(), lastPASS.c_str(), lastChannel, lastBSSID, true);

            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
                delay(200);
            }

            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("WiFiManager log: ✅ Reconnect lại thành công tới %s\n", lastSSID.c_str());
                LEDControl::setColor(0, 255, 0);
            } else {
                Serial.println("WiFiManager log: ❌ Reconnect thất bại, chờ lần kế tiếp...");
            }
        }
    }
}
