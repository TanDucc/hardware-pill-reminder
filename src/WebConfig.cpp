#include "WebConfig.h"
#include "FileStorage.h"
#include "LEDControl.h"

extern WebServer server; 

void handleRoot() {
  String html = "<h2>Nhap WiFi moi</h2>"
                "<form action='/save'>"
                "SSID: <input name='ssid'><br>"
                "Password: <input name='pass'><br>"
                "<input type='submit' value='Luu'>"
                "</form>";
  server.send(200, "text/html", html);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.length() > 0) {
    FileStorage::saveWiFiToSD(ssid, pass);
    server.send(200, "text/html", "✅ Da luu WiFi! ESP se khoi dong lai...");
    delay(2000);
    ESP.restart();
  } else {
    server.send(400, "text/html", "❌ SSID khong hop le");
  }
}

namespace WebConfig {
  void startAPConfig() {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(200);

    WiFi.mode(WIFI_AP);
    bool apStarted = WiFi.softAP("ESP32_Config", "12345678", 6, 0, 4);

    if (apStarted) {
      Serial.println("WebConfig log: ✅ AP mở thành công!");
      Serial.print("WebConfig log: 🌐 Truy cập: http://");
      Serial.println(WiFi.softAPIP());
      LEDControl::setColor(0, 0, 255);  // Blue
    } else {
      Serial.println("WebConfig log: ❌ AP mở thất bại!");
    }

    server.on("/", handleRoot);
    server.on("/save", handleSave);
    server.begin();
  }

  void handle() {
    server.handleClient();
  }
}
