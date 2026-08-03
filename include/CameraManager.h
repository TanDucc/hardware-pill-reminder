#pragma once
#include <Arduino.h>
#include "esp_camera.h"
#include "WiFiManager.h"
#include "WebConfig.h"

// ======================================== CAMERA STREAM ========================================
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

extern String cloud_name;
extern String upload_preset;
extern String upload_folder;

extern WebServer server;

namespace CameraManager {
    bool init();
    void handleStream();
    void uploadToCloudinary();
    void stop();
}
