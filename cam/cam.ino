/*
 * ============================================================================
 * Project : ESP32 WiFi Router & Classroom Occupancy Monitor
 * File    : cam/cam.ino
 *
 * Target  : ESP32-CAM (AI-Thinker module, OV2640 sensor)
 * Core    : arduino-esp32 v3.x (ESP-IDF 5.x)
 * Board   : "AI Thinker ESP32-CAM"  (FQBN: esp32:esp32:esp32cam)
 *
 * Endpoints served:
 *   GET /          - debug HTML page embedding the live stream
 *   GET /capture   - single JPEG frame  (pulled by person_counter.py)
 *   GET /stream    - MJPEG stream        (for live browser preview)
 *
 * person_counter.py usage:
 *   python person_counter.py --source esp32cam --cam-url http://<IP>
 *
 * Flashing notes:
 *   1. Hold the BOOT/IO0 button while powering on to enter flash mode.
 *   2. Select board "AI Thinker ESP32-CAM" in Arduino IDE.
 *   3. Upload.  Release BOOT.  Press RESET to run.
 *   4. Open Serial Monitor at 115200 baud to see the assigned IP address.
 * ============================================================================
 */

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

// ---------------------------------------------------------------------------
// WiFi credentials — set to your upstream network
// ---------------------------------------------------------------------------
#define WIFI_SSID     "Hostel_Guest"   // open network -> empty password
#define WIFI_PASS     ""               // leave empty for open networks

// Maximum time to wait for WiFi before rebooting (ms)
#define WIFI_TIMEOUT_MS  30000

// ---------------------------------------------------------------------------
// AI-Thinker ESP32-CAM pin map (OV2640)
// ---------------------------------------------------------------------------
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27

#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// GPIO4 is the white LED flash
#define FLASH_LED_PIN    4

// ---------------------------------------------------------------------------
// HTTP stream constants
// ---------------------------------------------------------------------------
#define PART_BOUNDARY  "gc0p4Jq0M2Yt08jU534c0p"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY =
    "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Two HTTP server handles:
//   port 80  -> capture + root
//   port 81  -> stream (separate so stream doesn't block capture requests)
static httpd_handle_t capture_server = NULL;
static httpd_handle_t stream_server  = NULL;

// ===========================================================================
//  Handler: GET /capture  — returns a single JPEG frame
// ===========================================================================
static esp_err_t capture_handler(httpd_req_t *req) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[CAM] Capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    esp_err_t res;
    if (fb->format == PIXFORMAT_JPEG) {
        res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    } else {
        uint8_t *jpg_buf = NULL;
        size_t   jpg_len = 0;
        bool ok = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
        if (!ok || !jpg_buf) {
            esp_camera_fb_return(fb);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        res = httpd_resp_send(req, (const char *)jpg_buf, jpg_len);
        free(jpg_buf);
    }

    esp_camera_fb_return(fb);
    return res;
}

// ===========================================================================
//  Handler: GET /stream  — MJPEG stream (runs until client disconnects)
// ===========================================================================
static esp_err_t stream_handler(httpd_req_t *req) {
    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char part_buf[64];
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[CAM] Stream: frame grab failed");
            break;
        }

        uint8_t *jpg_buf = NULL;
        size_t   jpg_len = 0;
        bool     need_free = false;

        if (fb->format == PIXFORMAT_JPEG) {
            jpg_buf = fb->buf;
            jpg_len = fb->len;
        } else {
            need_free = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
            if (!need_free) {
                esp_camera_fb_return(fb);
                continue;
            }
        }

        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, jpg_len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_len);
        }

        if (need_free) free(jpg_buf);
        esp_camera_fb_return(fb);

        if (res != ESP_OK) break;  // Client disconnected
    }
    return res;
}

// ===========================================================================
//  Handler: GET /  — simple HTML debug page
// ===========================================================================
static esp_err_t root_handler(httpd_req_t *req) {
    char ip_str[16];
    WiFi.localIP().toString().toCharArray(ip_str, sizeof(ip_str));

    char html[900];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<title>ESP32-CAM Debug</title>"
        "<style>body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px;}"
        "img{max-width:100%%;border:2px solid #444;border-radius:6px;}"
        "h1{font-size:1.2rem;margin-bottom:8px;}a{color:#7cf;}</style></head><body>"
        "<h1>ESP32-CAM Live Stream</h1>"
        "<img src='http://%s:81/stream' alt='MJPEG stream'/>"
        "<p>Snapshot: <a href='/capture' target='_blank'>/capture</a></p>"
        "<p>IP: <strong>%s</strong></p>"
        "</body></html>",
        ip_str, ip_str);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

// ===========================================================================
//  Start HTTP servers
// ===========================================================================
static void startCameraServers() {
    // Capture + root: port 80
    httpd_config_t cfg80      = HTTPD_DEFAULT_CONFIG();
    cfg80.server_port         = 80;
    cfg80.ctrl_port           = 32768;
    cfg80.max_open_sockets    = 4;
    cfg80.stack_size          = 8192;

    if (httpd_start(&capture_server, &cfg80) == ESP_OK) {
        httpd_uri_t cap_uri  = { "/capture", HTTP_GET, capture_handler, NULL };
        httpd_uri_t root_uri = { "/",        HTTP_GET, root_handler,    NULL };
        httpd_register_uri_handler(capture_server, &cap_uri);
        httpd_register_uri_handler(capture_server, &root_uri);
        Serial.println("[HTTP] Capture server started on port 80");
    } else {
        Serial.println("[HTTP] ERROR: capture server start failed!");
    }

    // Stream: port 81
    httpd_config_t cfg81      = HTTPD_DEFAULT_CONFIG();
    cfg81.server_port         = 81;
    cfg81.ctrl_port           = 32769;
    cfg81.max_open_sockets    = 2;
    cfg81.stack_size          = 8192;

    if (httpd_start(&stream_server, &cfg81) == ESP_OK) {
        httpd_uri_t str_uri = { "/stream", HTTP_GET, stream_handler, NULL };
        httpd_register_uri_handler(stream_server, &str_uri);
        Serial.println("[HTTP] Stream  server started on port 81");
    } else {
        Serial.println("[HTTP] ERROR: stream server start failed!");
    }
}

// ===========================================================================
//  Camera init
// ===========================================================================
static bool initCamera() {
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
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

    if (psramFound()) {
        config.frame_size   = FRAMESIZE_VGA;   // 640x480 - reliable for YOLOv8
        config.jpeg_quality = 10;
        config.fb_count     = 2;
        config.fb_location  = CAMERA_FB_IN_PSRAM;
        config.grab_mode    = CAMERA_GRAB_LATEST;
        Serial.println("[CAM] PSRAM found -> VGA, 2 frame buffers");
    } else {
        config.frame_size   = FRAMESIZE_SVGA;  // 800x600 fallback
        config.jpeg_quality = 12;
        config.fb_count     = 1;
        config.fb_location  = CAMERA_FB_IN_DRAM;
        Serial.println("[CAM] No PSRAM -> SVGA, 1 frame buffer (DRAM)");
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] Init FAILED: 0x%x\n", err);
        return false;
    }

    // Sensor tweaks for better detection quality
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_lenc(s, 1);
        s->set_raw_gma(s, 1);
        s->set_dcw(s, 1);
        s->set_hmirror(s, 0);
        s->set_vflip(s, 0);
        s->set_colorbar(s, 0);
    }

    Serial.println("[CAM] Camera initialized OK");
    return true;
}

// ===========================================================================
//  setup()
// ===========================================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("============================================");
    Serial.println("  ESP32-CAM  |  Classroom Occupancy Node  ");
    Serial.println("============================================");

    // Keep flash LED off
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, LOW);

    // Init camera
    if (!initCamera()) {
        Serial.println("[FATAL] Camera init failed - rebooting in 5 s");
        delay(5000);
        ESP.restart();
    }

    // Connect to WiFi
    Serial.printf("[WiFi] Connecting to '%s' ...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    if (strlen(WIFI_PASS) > 0) {
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    } else {
        WiFi.begin(WIFI_SSID);
    }

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 > WIFI_TIMEOUT_MS) {
            Serial.println("[WiFi] Timeout - rebooting");
            delay(1000);
            ESP.restart();
        }
        Serial.print(".");
        delay(500);
    }
    Serial.println();
    Serial.printf("[WiFi] Connected! IP: %s  RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());

    startCameraServers();

    Serial.println();
    Serial.println("----------- Ready -----------");
    Serial.printf("  Debug page : http://%s/\n",          WiFi.localIP().toString().c_str());
    Serial.printf("  Capture    : http://%s/capture\n",   WiFi.localIP().toString().c_str());
    Serial.printf("  Stream     : http://%s:81/stream\n", WiFi.localIP().toString().c_str());
    Serial.println();
    Serial.println("person_counter.py usage:");
    Serial.printf("  python person_counter.py --source esp32cam --cam-url http://%s\n",
                  WiFi.localIP().toString().c_str());
    Serial.println("-----------------------------");
}

// ===========================================================================
//  loop() - HTTP servers run in FreeRTOS tasks; we just watchdog WiFi here
// ===========================================================================
void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Lost connection - reconnecting...");
        WiFi.disconnect();
        delay(1000);
        if (strlen(WIFI_PASS) > 0) {
            WiFi.begin(WIFI_SSID, WIFI_PASS);
        } else {
            WiFi.begin(WIFI_SSID);
        }
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
            delay(500);
            Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n[WiFi] Reconnected! IP: %s\n",
                          WiFi.localIP().toString().c_str());
        } else {
            Serial.println("\n[WiFi] Reconnect failed - rebooting");
            delay(1000);
            ESP.restart();
        }
    }
    delay(5000);
}
