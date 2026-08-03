/* ==========  SMART PILLBOX - FINAL INTEGRATED CODE  ==========  */

#include "WiFiManager.h"
#include "LEDControl.h"
#include "FileStorage.h"
#include "WebConfig.h"
#include "CameraManager.h"
#include <WebServer.h>
#include "MAX98357_i2s.h"

#include <Wire.h>
#include "RTClib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <esp_sleep.h>

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <base64.h>

// ======================================== CẤU HÌNH PHẦN CỨNG ========================================
#define DS_SCL          42
#define DS_SDA          41
#define reed_pin        14  // Cảm biến từ: LOW = Đóng (có nam châm), HIGH = Mở

// ======================================== CẤU HÌNH SERVER ========================================
const char* host = "tanduc2409-esp32-yolo-api.hf.space"; // URL Hugging Face
const int httpsPort = 443;

// ===== Firestore Config =====
const char* fsProjectId = "smart-pillbox-54626";
const char* fsUserId    = "uSg4LL2orlPB8Yj7jtdnQ8n5pjm2";
const String fsIdToken  = ""; // Token nếu cần, để rỗng nếu public

// ======================================== BIẾN TOÀN CỤC ========================================
#define SLEEP_DELAY_MS 300000 // 5 phút = 300000 ms
#define NUM_SLOTS 3

RTC_DS3231 rtc;
WebServer server(80);

QueueHandle_t audioQueue;       
QueueHandle_t reedStateQueue;   
TaskHandle_t wifiCamTaskHandle = NULL;

static const char *AUDIO_CMD_STOP = "__AUDIO_STOP__";
bool isSDFound = true;
unsigned long lastPrint = 0;

// Biến lưu giờ thuốc tải từ Firestore
int morning_hour = -1, morning_minute = -1;
int noon_hour    = -1, noon_minute = -1;
int evening_hour = -1, evening_minute = -1;

// Trạng thái thuốc hiện tại trong hộp (True = Có thuốc, False = Rỗng)
// Mặc định ban đầu là True, sẽ được cập nhật chính xác khi khởi động
bool lastPillState[NUM_SLOTS] = {true, true, true}; 

struct PillAlarm {
    int hour;
    int minute;
    bool isPlayingAlarm;
    bool isOpenedNotified;
    unsigned long alarmTriggeredTime;
    bool isPhotoChecked;
    bool isDoneToday; // True: Đã uống thuốc hôm nay (sẽ không kêu nữa)
};

// Khởi tạo mảng báo thức
PillAlarm alarms[NUM_SLOTS] = {
    {7, 0, false, false, 0, false, false},   
    {11, 0, false, false, 0, false, false},  
    {18, 0, false, false, 0, false, false}   
};

// ======================================== FIRESTORE HELPERS ========================================

String buildFirestoreUrlForDate(const char* dateStr) {
  String url = String("https://firestore.googleapis.com/v1/projects/") +
               fsProjectId + "/databases/(default)/documents/medicine_times/" +
               fsUserId + "/days/" + dateStr;
  if (fsIdToken.length() > 0) url += "?access_token=" + fsIdToken;
  return url;
}

// Parse dữ liệu giờ hẹn từ JSON
bool parseFirestoreDocument(const String &payload) {
  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, payload)) return false;
  if (!doc.containsKey("fields")) return false;

  JsonObject fields = doc["fields"].as<JsonObject>();
  auto getInt = [&](const char* key, int &outVal) {
    if (!fields.containsKey(key)) return false;
    JsonVariant v = fields[key];
    if (v.containsKey("integerValue")) { outVal = atoi((const char*)v["integerValue"]); return true; }
    else if (v.containsKey("stringValue"))  { outVal = atoi((const char*)v["stringValue"]);  return true; }
    else if (v.is<int>()) { outVal = v.as<int>(); return true; }
    return false;
  };

  getInt("morning_hour", morning_hour); getInt("morning_minute", morning_minute);
  getInt("noon_hour", noon_hour);       getInt("noon_minute", noon_minute);
  getInt("evening_hour", evening_hour); getInt("evening_minute", evening_minute);
  return true;
}

// Tải lịch thuốc hôm nay
bool loadMedicineTimeForToday() {
  DateTime now = rtc.now();
  char dateStr[11];
  sprintf(dateStr, "%04d-%02d-%02d", now.year(), now.month(), now.day());

  Serial.printf("🔍 GET Firestore ngày: %s\n", dateStr);
  String url = buildFirestoreUrlForDate(dateStr);

  WiFiClientSecure *client = new WiFiClientSecure();
  client->setInsecure();
  HTTPClient http;
  
  bool ok = false;
  if (http.begin(*client, url)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
      if (parseFirestoreDocument(http.getString())) {
        Serial.println("✅ Đã cập nhật giờ thuốc thành công.");
        Serial.printf("   Morning: %02d:%02d\n", morning_hour, morning_minute);
        Serial.printf("   Noon:    %02d:%02d\n", noon_hour, noon_minute);
        Serial.printf("   Evening: %02d:%02d\n", evening_hour, evening_minute);
        ok = true;
      }
    } else {
      Serial.printf("⚠️ Không tìm thấy lịch (HTTP %d)\n", httpCode);
    }
    http.end();
  }
  delete client;
  return ok;
}

void updateAlarmsFromFirestore() {
  if (morning_hour >= 0)  { alarms[0].hour = morning_hour;  alarms[0].minute = morning_minute; }
  if (noon_hour >= 0)     { alarms[1].hour = noon_hour;     alarms[1].minute = noon_minute; }
  if (evening_hour >= 0)  { alarms[2].hour = evening_hour;  alarms[2].minute = evening_minute; }
}

// Gửi giờ đã uống (PATCH esp32_...)
void uploadTakenTime(String timeSlot, int hour, int minute) {
  if (WiFi.status() != WL_CONNECTED) return;
  DateTime now = rtc.now();
  char dateStr[11];
  sprintf(dateStr, "%04d-%02d-%02d", now.year(), now.month(), now.day());

  String fieldHour = "esp32_" + timeSlot + "_hour";
  String fieldMinute = "esp32_" + timeSlot + "_minute";
  String url = buildFirestoreUrlForDate(dateStr);
  url += (url.indexOf('?') == -1 ? "?" : "&");
  url += "updateMask.fieldPaths=" + fieldHour + "&updateMask.fieldPaths=" + fieldMinute;

  StaticJsonDocument<512> doc;
  JsonObject fields = doc.createNestedObject("fields");
  fields[fieldHour]["integerValue"] = String(hour);
  fields[fieldMinute]["integerValue"] = String(minute);
  String jsonPayload;
  serializeJson(doc, jsonPayload);

  WiFiClientSecure *client = new WiFiClientSecure();
  client->setInsecure();
  HTTPClient http;
  if (http.begin(*client, url)) {
    http.addHeader("Content-Type", "application/json");
    http.sendRequest("PATCH", jsonPayload);
    http.end();
  }
  delete client;
}

// Ghi log hành động vào document ngày (Dynamic Field)
void logActionToDayDocument(int slotIndex, String action) {
  if (WiFi.status() != WL_CONNECTED) return;
  DateTime now = rtc.now();
  char dateStr[11];
  sprintf(dateStr, "%04d-%02d-%02d", now.year(), now.month(), now.day());

  // Tạo tên field theo thời gian để không bị trùng: log_HH_MM_SS_khayX
  char fieldName[32];
  sprintf(fieldName, "log_%02d_%02d_%02d_khay%d", now.hour(), now.minute(), now.second(), slotIndex + 1);

  String url = buildFirestoreUrlForDate(dateStr);
  url += (url.indexOf('?') == -1 ? "?" : "&");
  url += "updateMask.fieldPaths=" + String(fieldName);

  StaticJsonDocument<256> doc;
  JsonObject fields = doc.createNestedObject("fields");
  fields[fieldName]["stringValue"] = action;
  String jsonPayload;
  serializeJson(doc, jsonPayload);

  Serial.printf("📝 Logging: %s -> %s\n", fieldName, action.c_str());

  WiFiClientSecure *client = new WiFiClientSecure();
  client->setInsecure();
  HTTPClient http;
  if (http.begin(*client, url)) {
    http.addHeader("Content-Type", "application/json");
    http.sendRequest("PATCH", jsonPayload);
    http.end();
  }
  delete client;
}

String getSlotName(int slotIndex) {
  if (slotIndex == 0) return "morning";
  if (slotIndex == 1) return "noon";
  return "evening";
}

// ======================================== AI & IMAGE PROCESSING ========================================

// Gửi ảnh lên Hugging Face
// Hàm gửi ảnh và nhận kết quả (Đã sửa lỗi đọc phản hồi -76)
// Hàm gửi ảnh dạng Binary (Multipart) - Không dùng Base64
// Hàm gửi ảnh Base64 (Server yêu cầu bắt buộc định dạng này)
String uploadToHuggingFaceAndGetResult() {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("❌ Lỗi Camera: Không chụp được ảnh");
        return "";
    }

    // --- (Tùy chọn) Lưu ảnh vào thẻ nhớ trước khi gửi ---
    // savePhotoToSD(fb); 
    // ----------------------------------------------------

    Serial.printf("📸 Ảnh: %d bytes. Đang mã hoá Base64...\n", fb->len);
    
    // Mã hoá Base64
    String base64Image = base64::encode(fb->buf, fb->len);
    esp_camera_fb_return(fb); // Giải phóng RAM Camera ngay lập tức

    // Tạo JSON Body
    String jsonHead = "{\"data\": [\"";
    String jsonTail = "\"]}";
    size_t contentLength = jsonHead.length() + base64Image.length() + jsonTail.length();

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20000); // Timeout 20s

    Serial.println("📤 Đang kết nối Server...");
    if (!client.connect(host, httpsPort)) {
        Serial.println("❌ Kết nối thất bại!");
        return "";
    }

    // Gửi HTTP Request
    client.println("POST /api/predict HTTP/1.1");
    client.println("Host: " + String(host));
    client.println("Content-Type: application/json");
    client.println("Connection: close"); 
    client.print("Content-Length: "); client.println(contentLength);
    client.println(); 

    // Gửi Body (Chia nhỏ gói tin để gửi ổn định hơn)
    client.print(jsonHead);
    int chunkSize = 2048; // Gửi từng gói 2KB
    int len = base64Image.length();
    for (int i = 0; i < len; i += chunkSize) {
        int remain = len - i;
        client.print(base64Image.substring(i, i + (remain < chunkSize ? remain : chunkSize)));
    }
    client.print(jsonTail);

    Serial.println("⏳ Đã gửi xong. Đang chờ phản hồi...");

    // --- ĐỌC PHẢN HỒI (BỎ QUA LỖI -76) ---
    String responseBody = "";
    bool bodyStarted = false;
    unsigned long startWait = millis();

    while (client.connected() || client.available()) {
        if (client.available()) {
            String line = client.readStringUntil('\n');
            if (line == "\r") { 
                bodyStarted = true; // Hết header, bắt đầu vào Body
                continue; 
            }
            if (bodyStarted) {
                responseBody += line;
            }
        }
        // Timeout an toàn
        if (millis() - startWait > 15000) {
            Serial.println("⚠️ Hết thời gian chờ (Timeout)");
            break;
        }
    }
    client.stop();

    Serial.println("✅ Phản hồi từ Server:");
    Serial.println(responseBody);

    // Trích xuất JSON (Lọc bỏ các ký tự rác nếu có)
    int jsonStart = responseBody.indexOf('{');
    int jsonEnd = responseBody.lastIndexOf('}');
    
    if (jsonStart != -1 && jsonEnd != -1) {
        return responseBody.substring(jsonStart, jsonEnd + 1);
    }
    
    // Nếu API trả về mảng [...]
    jsonStart = responseBody.indexOf('[');
    jsonEnd = responseBody.lastIndexOf(']');
    if (jsonStart != -1 && jsonEnd != -1) {
        return responseBody.substring(jsonStart, jsonEnd + 1);
    }

    // Nếu server trả về lỗi Internal Server Error thì trả về rỗng để code không bị treo
    if (responseBody.indexOf("Internal Server Error") != -1) {
        Serial.println("❌ Server bị lỗi (500). Kiểm tra lại định dạng gửi.");
        return "";
    }

    return responseBody;
}
// Kiểm tra kết quả AI
// Hàm kiểm tra thuốc đã được cập nhật để đọc JSON chuẩn
bool isSlotHasPill(const String& result, int slotIndex) {
    // result ví dụ: {"khay1":"Co thuoc", "khay2":null, "khay3":"Khong thuoc"}
    
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, result);

    if (error) {
        Serial.print("⚠️ Lỗi parse JSON kết quả AI: ");
        Serial.println(error.c_str());
        return false;
    }

    // Tạo key: khay1, khay2, khay3
    String key = "khay" + String(slotIndex + 1); 

    // Kiểm tra xem key có tồn tại và có giá trị không
    if (doc.containsKey(key)) {
        // Nếu giá trị là null -> Coi như không có thuốc (hoặc không phát hiện gì)
        if (doc[key].isNull()) {
            return false; 
        }
        
        // Lấy giá trị chuỗi
        String value = doc[key].as<String>();
        
        // Kiểm tra nếu chứa chữ "Co thuoc"
        if (value.indexOf("Co thuoc") != -1) {
            return true;
        }
    }

    return false;
}

// Phân tích hành động người dùng (Gọi khi đóng nắp)
void analyzeUserAction() {
  Serial.println("🔍 Phân tích hành động (AI)...");
  String result = uploadToHuggingFaceAndGetResult();
  if (result == "") { Serial.println("⚠️ AI Error"); return; }

  for (int i = 0; i < NUM_SLOTS; i++) {
    bool currentHasPill = isSlotHasPill(result, i);
    bool previousHasPill = lastPillState[i];
    String action = "";

    // 1. Logic phát hiện hành động
    if (previousHasPill && !currentHasPill) {
      action = "Lay thuoc";
      // QUAN TRỌNG: Uống rồi -> Hủy báo thức hôm nay + Ghi giờ uống
      alarms[i].isDoneToday = true;
      Serial.printf("✅ Khay %d đã uống -> Hủy báo thức.\n", i+1);
      
      DateTime now = rtc.now();
      uploadTakenTime(getSlotName(i), now.hour(), now.minute());
    } 
    else if (!previousHasPill && currentHasPill) {
      action = "Bo thuoc vao";
      alarms[i].isDoneToday = false; // Reset để sẵn sàng báo thức
    }

    // 2. Ghi log và cập nhật trạng thái
    if (action != "") {
      logActionToDayDocument(i, action);
      lastPillState[i] = currentHasPill;
    } else {
        Serial.printf("ℹ️ Khay %d: Không đổi (%s)\n", i+1, currentHasPill?"Có":"Không");
    }
  }
}

// ======================================== AUDIO HELPER ========================================
void playAlarmSound(const char *filename) {
    if (isSDFound) MAX98357::playWavDoubleBuffer(filename);
    else MAX98357::playBeep();  
}

// ======================================== TASKS ========================================

// --- TASK 1: QUẢN LÝ THỜI GIAN VÀ BÁO THỨC ---
void TaskRTC(void *pvParameters) {
    static int lastDay = -1;
    static bool isDataSynced = false;

    while (1) {
        DateTime now = rtc.now();
        int state = digitalRead(reed_pin); // LOW = Đóng

        // In thời gian (Debug)
        if (!MAX98357::isPlayingDoubleBuffer() && millis() - lastPrint >= 1000) {
            lastPrint = millis();
            Serial.printf("⏰ %02d:%02d:%02d | Done: %d-%d-%d\n", 
                now.hour(), now.minute(), now.second(), 
                alarms[0].isDoneToday, alarms[1].isDoneToday, alarms[2].isDoneToday);
        }

        // --- XỬ LÝ SANG NGÀY MỚI ---
        if (now.day() != lastDay) {
            lastDay = now.day();
            isDataSynced = false;
            // Reset cờ uống thuốc
            for(int k=0; k<NUM_SLOTS; k++) alarms[k].isDoneToday = false;
            Serial.println("📅 Sang ngày mới -> Reset trạng thái.");
        }

        // Thử tải lịch nếu chưa xong
        if (!isDataSynced && WiFi.status() == WL_CONNECTED) {
            if (loadMedicineTimeForToday()) {
                updateAlarmsFromFirestore();
                isDataSynced = true;
            }
        }

        // --- LOGIC BÁO THỨC ---
        for (int i = 0; i < NUM_SLOTS; i++) {
            // Trigger: Đúng giờ + Chưa uống hôm nay
            if (now.hour() == alarms[i].hour && now.minute() == alarms[i].minute) {
                if (state == LOW && !alarms[i].isPlayingAlarm && !alarms[i].isDoneToday) {
                    Serial.printf("🔊 Báo thức khay %d!\n", i + 1);
                    const char *msg = "/alarm.wav";
                    xQueueSend(audioQueue, &msg, portMAX_DELAY);
                    alarms[i].isPlayingAlarm = true;
                    alarms[i].isOpenedNotified = false;
                } 
                // Nếu đang kêu mà mở nắp -> Tắt
                else if (state == HIGH && !alarms[i].isOpenedNotified && alarms[i].isPlayingAlarm) {
                    Serial.println("🔕 Đã mở nắp -> Tắt chuông.");
                    const char *cmd = AUDIO_CMD_STOP;
                    xQueueSend(audioQueue, &cmd, 0);
                    
                    alarms[i].isOpenedNotified = true;
                    alarms[i].isDoneToday = true; // Coi như đã xử lý xong lần này
                    
                    // Ghi nhận giờ mở nắp (phòng trường hợp AI chưa kịp quét)
                    uploadTakenTime(getSlotName(i), now.hour(), now.minute());
                }
            }
            // Reset biến isPlayingAlarm khi qua phút để tránh kẹt
            if (alarms[i].isPlayingAlarm && now.minute() != alarms[i].minute) {
                alarms[i].isPlayingAlarm = false;
            }
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}

// --- TASK 2: PHÁT ÂM THANH ---
void TaskAudio(void *pvParameters) {
    const char *fileToPlay = nullptr;
    bool wasWifiCamSuspended = false;
    for (;;) {
        if (xQueueReceive(audioQueue, &fileToPlay, 10 / portTICK_PERIOD_MS)) {
            if (fileToPlay == AUDIO_CMD_STOP) {
                MAX98357::stopWavDoubleBuffer();
                if (wasWifiCamSuspended && wifiCamTaskHandle) {
                    vTaskDelay(pdMS_TO_TICKS(50)); vTaskResume(wifiCamTaskHandle); wasWifiCamSuspended = false;
                }
                continue;
            }
            if (fileToPlay != nullptr) {
                if (wifiCamTaskHandle) { vTaskSuspend(wifiCamTaskHandle); wasWifiCamSuspended = true; vTaskDelay(50); }
                playAlarmSound(fileToPlay);
                while (MAX98357::isPlayingDoubleBuffer()) {
                    const char *cmd = nullptr;
                    if (xQueueReceive(audioQueue, &cmd, 0)) {
                        if (cmd == AUDIO_CMD_STOP) { MAX98357::stopWavDoubleBuffer(); break; }
                    }
                    vTaskDelay(1);
                }
                if (wasWifiCamSuspended && wifiCamTaskHandle) { vTaskDelay(50); vTaskResume(wifiCamTaskHandle); wasWifiCamSuspended = false; }
            }
        }
        vTaskDelay(5);
    }
}

// --- TASK 3: WIFI & CAMERA & KHỞI TẠO LOGIC ---
void TaskWiFiCam(void *pvParameters) {
    if(isSDFound) {
        FileStorage::loadWiFiFromSD();
        bool connected = false;
        if (!wifiList.empty()) connected = WiFiManager::connectBestWiFi();
        if (!connected) WebConfig::startAPConfig();

        unsigned long t = millis();
        while (connected && WiFi.status() != WL_CONNECTED && millis() - t < 20000) delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("✅ WiFi Connected!");
        // 1. Tải lịch
        if (loadMedicineTimeForToday()) updateAlarmsFromFirestore();
        
        // 2. Camera Init
        if (CameraManager::init()) {
            server.on("/stream", HTTP_GET, CameraManager::handleStream);
            server.begin();
            
           
            Serial.print("🌐 Mở trình duyệt tại: http://");
            Serial.print(WiFi.localIP());
            Serial.println("/stream");
            // 3. Quét trạng thái thuốc ban đầu (Để làm mốc so sánh)
            Serial.println("📸 Init Scan for Pill States...");
            String res = uploadToHuggingFaceAndGetResult();
            if(res != "") {
                for(int i=0; i<NUM_SLOTS; i++) {
                    lastPillState[i] = isSlotHasPill(res, i);
                    Serial.printf("   Box %d: %s\n", i+1, lastPillState[i]?"Full":"Empty");
                }
            }
        }
    } else {
        Serial.println("❌ WiFi Failed");
    }

    for (;;) {
        WebConfig::handle();
        WiFiManager::monitorConnection();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// --- TASK 4: SERIAL INPUT (Giữ nguyên) ---
void TaskSerialInput(void *pvParameters) {
    for (;;) {
        if (Serial.available()) {
            String input = Serial.readStringUntil('\n'); input.trim();
            // Xử lý nếu cần...
        }
        vTaskDelay(100);
    }
}

// --- TASK 5: AUTO SLEEP & PHÁT HIỆN HÀNH ĐỘNG ---
void TaskAutoSleep(void *pvParameters) {
    unsigned long lastCloseTime = 0;
    bool wasClosed = false;
    bool inLightSleep = false;

    // Đọc trạng thái ban đầu
    if(digitalRead(reed_pin) == LOW) wasClosed = true;

    for (;;) {
        int state = digitalRead(reed_pin); // LOW = Đóng

        // --- SỰ KIỆN: ĐÓNG NẮP (Mở -> Đóng) ---
        if (state == LOW && !wasClosed) {
            wasClosed = true;
            lastCloseTime = millis();
            Serial.println("📦 Hộp đã đóng.");

            // Chờ ổn định rồi phân tích hành động
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (WiFi.status() == WL_CONNECTED) {
                analyzeUserAction(); // <--- GỌI AI Ở ĐÂY
            }
            Serial.println("⏳ Start sleep timer (5 min)...");
        } 
        
        // --- SỰ KIỆN: MỞ NẮP (Đóng -> Mở) ---
        else if (state == HIGH && wasClosed) {
            wasClosed = false;
            if (inLightSleep) {
                Serial.println("🔓 Wake up system!");
                inLightSleep = false;
                WiFi.setSleep(WIFI_PS_MIN_MODEM);
                CameraManager::init();
            }
        }

        // --- CHECK SLEEP ---
        if (wasClosed && !inLightSleep && (millis() - lastCloseTime >= SLEEP_DELAY_MS)) {
            Serial.println("💤 Entering Light Sleep...");
            CameraManager::stop();
            vTaskDelay(100);
            WiFi.setSleep(WIFI_PS_MAX_MODEM);
            esp_sleep_enable_ext0_wakeup((gpio_num_t)reed_pin, HIGH);
            inLightSleep = true;
            esp_light_sleep_start();
            Serial.println("🌅 Woke up from sleep!");
        }

        vTaskDelay(1000);
    }
}

// ======================================== SETUP & LOOP ========================================
void setup() {
    Serial.begin(115200);
    Wire.begin(DS_SDA, DS_SCL);
    pinMode(reed_pin, INPUT_PULLUP);
    delay(1000);

    // RTC
    if (!rtc.begin()) { Serial.println("❌ No DS3231"); while (1); }
    // Chỉ bật dòng dưới 1 lần để set giờ, sau đó comment lại
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); 

    if (rtc.lostPower()) {
        Serial.println("⚠️ RTC Lost Power -> Reset time.");
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    LEDControl::init();
    if (!FileStorage::initSD()) { Serial.println("❌ SD Fail"); isSDFound = false; }
    else Serial.println("✅ SD OK");

    if(psramFound()) Serial.printf("✅ PSRAM: %d bytes\n", ESP.getPsramSize());

    audioQueue = xQueueCreate(5, sizeof(char*));
    reedStateQueue = xQueueCreate(1, sizeof(int));

    // Stack sizes được tăng lên để xử lý HTTPS & JSON
    xTaskCreatePinnedToCore(TaskRTC, "RTC Task", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(TaskAudio, "Audio Task", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(TaskWiFiCam, "WiFiCam Task", 16384, NULL, 3, &wifiCamTaskHandle, 0);
    xTaskCreatePinnedToCore(TaskSerialInput, "Serial Input", 4096, NULL, 1, NULL, 1);
    // TaskAutoSleep tăng stack vì chứa hàm analyzeUserAction (gọi HTTPS)
    xTaskCreatePinnedToCore(TaskAutoSleep, "Auto Sleep", 16384, NULL, 1, NULL, 1); 
}

void loop() {
    vTaskDelay(1000);
}