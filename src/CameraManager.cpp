#include "CameraManager.h"

// ⚡ Cấu hình chân camera OV2640 trên ESP32-S3 (DevKit board)
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5

#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM       8
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

String cloud_name    = "daf8drhef";
String upload_preset = "unsigned_preset";
String upload_folder = "esp32_uploads";

namespace CameraManager {

    bool init() {
        camera_config_t config;
        config.ledc_channel = LEDC_CHANNEL_0;
        config.ledc_timer   = LEDC_TIMER_0;
        config.pin_d0       = Y2_GPIO_NUM;
        config.pin_d1       = Y3_GPIO_NUM;
        config.pin_d2       = Y4_GPIO_NUM;
        config.pin_d3       = Y5_GPIO_NUM;
        config.pin_d4       = Y6_GPIO_NUM;
        config.pin_d5       = Y7_GPIO_NUM;
        config.pin_d6       = Y8_GPIO_NUM;
        config.pin_d7       = Y9_GPIO_NUM;
        config.pin_xclk     = XCLK_GPIO_NUM;
        config.pin_pclk     = PCLK_GPIO_NUM;
        config.pin_vsync    = VSYNC_GPIO_NUM;
        config.pin_href     = HREF_GPIO_NUM;
        config.pin_sscb_sda = SIOD_GPIO_NUM;
        config.pin_sscb_scl = SIOC_GPIO_NUM;
        config.pin_pwdn     = PWDN_GPIO_NUM;
        config.pin_reset    = RESET_GPIO_NUM;
        config.xclk_freq_hz = 20000000;  // Xung 20MHz ổn định
        config.pixel_format = PIXFORMAT_JPEG; 
        config.fb_location = CAMERA_FB_IN_PSRAM; 
        config.grab_mode   = CAMERA_GRAB_LATEST; // lấy frame mới nhất để giảm trễ

        if (psramFound()) {
            Serial.println("CameraManager log: PSRAM Ok");
            config.frame_size   = FRAMESIZE_QVGA;   // 640x480, vừa đủ stream mượt
            config.jpeg_quality = 12;              // giảm chất lượng để tăng tốc
            config.fb_count     = 2;               // double buffer để giảm lag
        } else {
            Serial.println("CameraManager log: No PSRAM");
            config.frame_size   = FRAMESIZE_QVGA;  // 320x240 cho mượt
            config.jpeg_quality = 15;
            config.fb_count     = 1;
        }

        esp_camera_deinit(); 
        esp_err_t err = esp_camera_init(&config);
        if (err != ESP_OK) {
            Serial.printf("CameraManager log: ❌ Camera init thất bại! Err=0x%x\n", err);
            return false;
        }
        Serial.println("CameraManager log: ✅ Camera init thành công!");
        return true;
    }

    void handleStream() {
        WiFiClient client = server.client();
        if (!client.connected()) return;

        client.println("HTTP/1.1 200 OK");
        client.printf("Content-Type: %s\r\n\r\n", STREAM_CONTENT_TYPE);

        while (client.connected()) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (!fb) {
                Serial.println("❌ Camera capture failed");
                break;
            }

            client.print(STREAM_BOUNDARY);
            client.printf(STREAM_PART, fb->len, millis()/1000, millis()%1000*1000);
            client.write(fb->buf, fb->len);
            client.print("\r\n");

            esp_camera_fb_return(fb);

            // KHÔNG nên dùng delay lớn -> thay bằng yield()
            // yield để tránh block
            if (!client.connected()) break;
            // small delay to reduce CPU hogging; tune to desired fps
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (client.connected()) client.stop();
    }

    void uploadToCloudinary() {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("CameraManager log: ❌ Không thể chụp ảnh!");
            return;
        }

        Serial.println("CameraManager log: 📸 Ảnh đã chụp, đang upload...");

        WiFiClientSecure client;
        client.setInsecure();  // Bỏ xác thực SSL
        if (!client.connect("api.cloudinary.com", 443)) {
            Serial.println("CameraManager log: ❌ Không kết nối được Cloudinary!");
            esp_camera_fb_return(fb);
            return;
        }

        String boundary = "----ESP32Boundary";
        String head = "--" + boundary + "\r\n"
                        "Content-Disposition: form-data; name=\"upload_preset\"\r\n\r\n" +
                        upload_preset + "\r\n" +
                        "--" + boundary + "\r\n"
                        "Content-Disposition: form-data; name=\"folder\"\r\n\r\n" +
                        upload_folder + "\r\n" +
                        "--" + boundary + "\r\n"
                        "Content-Disposition: form-data; name=\"file\"; filename=\"esp32.jpg\"\r\n"
                        "Content-Type: image/jpeg\r\n\r\n";

        String tail = "\r\n--" + boundary + "--\r\n";
        int contentLength = head.length() + fb->len + tail.length();

        // Gửi header HTTP thủ công
        client.println("CameraManager log: POST /v1_1/" + cloud_name + "/image/upload HTTP/1.1");
        client.println("CameraManager log: Host: api.cloudinary.com");
        client.println("CameraManager log: Content-Type: multipart/form-data; boundary=" + boundary);
        client.println("CameraManager log: Content-Length: " + String(contentLength));
        client.println("CameraManager log: Connection: close");
        client.println();

        // Gửi body
        client.print(head);
        client.write(fb->buf, fb->len);
        client.print(tail);

        // Đọc phản hồi
        String response;
        while (client.connected() || client.available()) {
            if (client.available()) {
            String line = client.readStringUntil('\n');
            response += line + "\n";
            }
        }

        Serial.println("CameraManager log: ✅ Upload completed!");
        Serial.println(response);

        esp_camera_fb_return(fb);
        client.stop();
    }

    void stop() {
        esp_camera_deinit();
        Serial.println("CameraManager log: Camera stopped.");
    }
}
