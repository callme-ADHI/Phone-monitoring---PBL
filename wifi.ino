/*
 * ============================================================================
 * Project: ESP32 WiFi Router & Occupancy Telemetry Transmitter
 * File:    wifi.ino
 *
 * Target:
 *   ESP32 Dev Module (standard WROOM-32)
 *   Targeted Core: arduino-esp32 v3.x (v3.0.0 through v3.3.x, ESP-IDF 5.x based)
 *
 * Architecture:
 *   - Creates SoftAP "PBL-MC" for student phones to connect to.
 *   - Connects upstream to "VoiDX" for real Internet access with lwIP NAPT.
 *   - Promiscuous 802.11 sniffer inspects MAC headers to monitor device usage
 *     without touching unencrypted/encrypted payload content (privacy-first).
 *   - Telemetry is streamed to the external Web Dashboard (Next.js) running
 *     on the local network via HTTP POST every 1.5 seconds.
 *   - Web UI is completely removed from the board for maximum heap and speed.
 * ============================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <esp_netif.h>

// ----------------------------------------------------------------------------
// Configuration Settings
// ----------------------------------------------------------------------------
#define SERIAL_BAUD       115200

// Set to 1 to enable NAPT routing. Set to 0 to disable routing entirely.
#define ENABLE_NAPT       1

// SoftAP settings (phones connect here)
static const char* AP_SSID = "PBL-MC";
static const char* AP_PASS = "123456";    // 6 chars: WPA2 requires 8+. Handled explicitly below.
static const int   AP_CHANNEL = 1;
static const int   AP_MAX_CLIENTS = 10;
static const IPAddress apIP(192, 168, 4, 1);
static const IPAddress apNetmask(255, 255, 255, 0);

// Upstream WiFi settings (Internet connection)
static const char* STA_SSID = "VoiDX";
static const char* STA_PASS = "sunisgood";

// External Web Dashboard server (Where the Next.js 'web' app is running)
static const char* DASHBOARD_HOST = "10.225.255.190"; // Host computer's IP on VoiDX
static const int   DASHBOARD_PORT = 3000;
static const char* DASHBOARD_PATH = "/api/esp32";
#define POST_INTERVAL_MS 1500

// Traffic Analysis thresholds (rolling 10-second window)
#define ROLLING_WINDOW_SECS 10
#define IDLE_THRESHOLD_BPS  1024      // < 1 KB/s -> idle
#define LIGHT_THRESHOLD_BPS 51200     // 1 KB/s to 50 KB/s -> light, > 50 KB/s -> active

// ----------------------------------------------------------------------------
// NAPT / Routing Linkage
// ----------------------------------------------------------------------------
#if ENABLE_NAPT
  #if __has_include("lwip/lwip_napt.h")
    #include "lwip/lwip_napt.h"
  #elif __has_include("lwip/ip4_napt.h")
    #include "lwip/ip4_napt.h"
  #endif

  extern "C" {
    // Weak linkage enables graceful build even if the underlying liblwip.a lacks NAPT
    void ip_napt_enable(uint32_t addr, int enable) __attribute__((weak));
  }
#endif

// ----------------------------------------------------------------------------
// Device Tracking Structure
// ----------------------------------------------------------------------------
#define MAX_TRACKED_DEVICES 32

struct DeviceStats {
  uint8_t  mac[6];
  uint32_t anon_hash;
  char     anon_id[7];       // 6 hex characters + null terminator
  uint32_t up_pkts;
  uint32_t up_bytes;
  uint32_t down_pkts;
  uint32_t down_bytes;
  uint32_t last_seen_ms;

  // Rolling rate calculation (1-second byte buckets)
  uint32_t sec_bytes[ROLLING_WINDOW_SECS];
  uint32_t recent_bytes_sum;
  uint32_t recent_rate_bps;   // Bytes per second over last ROLLING_WINDOW_SECS
  bool     in_use;
};

static DeviceStats tracked_devices[MAX_TRACKED_DEVICES];
static portMUX_TYPE tracker_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t boot_salt = 0;
static uint8_t  ap_mac[6];
static uint8_t  curr_bucket_idx = 0;
static uint32_t last_bucket_tick = 0;
static uint32_t last_post_tick = 0;
static uint32_t sta_last_retry_ms = 0;

// System state
static bool     napt_active = false;
static bool     sta_connected = false;

// Optional lightweight local fallback server on port 80
WebServer server(80);

// ----------------------------------------------------------------------------
// Device Privacy: Anonymization Hashing
// ----------------------------------------------------------------------------
static uint32_t hash_mac(const uint8_t* mac) {
  // FNV-1a 32-bit hash seeded with a per-boot hardware random salt
  uint32_t h = 2166136261u ^ boot_salt;
  for (int i = 0; i < 6; i++) {
    h ^= mac[i];
    h *= 16777619u;
  }
  return h;
}

static DeviceStats* find_or_create_device(const uint8_t* mac, uint32_t now) {
  for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
    if (tracked_devices[i].in_use && memcmp(tracked_devices[i].mac, mac, 6) == 0) {
      return &tracked_devices[i];
    }
  }

  int target_slot = -1;
  uint32_t oldest_seen = UINT32_MAX;

  for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
    if (!tracked_devices[i].in_use) {
      target_slot = i;
      break;
    }
    if (tracked_devices[i].last_seen_ms < oldest_seen) {
      oldest_seen = tracked_devices[i].last_seen_ms;
      target_slot = i;
    }
  }

  if (target_slot >= 0) {
    DeviceStats* dev = &tracked_devices[target_slot];
    memset(dev, 0, sizeof(DeviceStats));
    memcpy(dev->mac, mac, 6);
    dev->anon_hash = hash_mac(mac);
    snprintf(dev->anon_id, sizeof(dev->anon_id), "%06X", (unsigned int)(dev->anon_hash & 0x00FFFFFF));
    dev->last_seen_ms = now;
    dev->in_use = true;
    return dev;
  }
  return nullptr;
}

// ----------------------------------------------------------------------------
// Promiscuous Sniffer: 802.11 Header Inspection Only
// Strictly inspects unencrypted MAC headers; payload is NEVER accessed.
// ----------------------------------------------------------------------------
static void wifi_promiscuous_rx_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_DATA) return;

  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* payload = pkt->payload;
  uint16_t len = pkt->rx_ctrl.sig_len;

  if (len < 24) return;

  uint8_t fc0 = payload[0];
  uint8_t fc1 = payload[1];

  // Data frames only (Type 0b10)
  if ((fc0 & 0x0C) != 0x08) return;

  bool to_ds   = (fc1 & 0x01) != 0;
  bool from_ds = (fc1 & 0x02) != 0;

  const uint8_t* addr1 = payload + 4;
  const uint8_t* addr2 = payload + 10;

  // At least one address must be our AP's BSSID
  if (to_ds && !from_ds) {
    // Uplink: Addr1 must be our AP MAC
    if (memcmp(addr1, ap_mac, 6) != 0) return;

    uint32_t now = millis();
    portENTER_CRITICAL_ISR(&tracker_mux);
    DeviceStats* dev = find_or_create_device(addr2, now);
    if (dev) {
      dev->up_pkts++;
      dev->up_bytes += len;
      dev->sec_bytes[curr_bucket_idx] += len;
      dev->last_seen_ms = now;
    }
    portEXIT_CRITICAL_ISR(&tracker_mux);
  }
  else if (!to_ds && from_ds) {
    // Downlink: Addr2 must be our AP MAC
    if (memcmp(addr2, ap_mac, 6) != 0) return;

    uint32_t now = millis();
    portENTER_CRITICAL_ISR(&tracker_mux);
    DeviceStats* dev = find_or_create_device(addr1, now);
    if (dev) {
      dev->down_pkts++;
      dev->down_bytes += len;
      dev->sec_bytes[curr_bucket_idx] += len;
      dev->last_seen_ms = now;
    }
    portEXIT_CRITICAL_ISR(&tracker_mux);
  }
}

// ----------------------------------------------------------------------------
// Rate & Usage Calculation
// ----------------------------------------------------------------------------
static void update_rolling_rates() {
  portENTER_CRITICAL(&tracker_mux);

  uint8_t next_bucket = (curr_bucket_idx + 1) % ROLLING_WINDOW_SECS;

  for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
    if (!tracked_devices[i].in_use) continue;
    DeviceStats* dev = &tracked_devices[i];

    uint32_t sum = 0;
    for (int b = 0; b < ROLLING_WINDOW_SECS; b++) {
      sum += dev->sec_bytes[b];
    }
    dev->recent_bytes_sum = sum;
    dev->recent_rate_bps = sum / ROLLING_WINDOW_SECS;
    dev->sec_bytes[next_bucket] = 0;
  }

  curr_bucket_idx = next_bucket;
  portEXIT_CRITICAL(&tracker_mux);
}

static const char* get_usage_label(uint32_t rate_bps, uint32_t idle_ms) {
  if (idle_ms > (ROLLING_WINDOW_SECS * 1000UL) || rate_bps < IDLE_THRESHOLD_BPS) {
    return "idle";
  } else if (rate_bps <= LIGHT_THRESHOLD_BPS) {
    return "light";
  } else {
    return "active";
  }
}

// ----------------------------------------------------------------------------
// NAPT Router Setup
// ----------------------------------------------------------------------------
static bool init_napt_routing() {
#if ENABLE_NAPT
  if (ip_napt_enable != nullptr) {
    ip_napt_enable((uint32_t)apIP, 1);
    Serial.println(F("[ROUTER] NAPT enabled successfully on SoftAP interface!"));

    IPAddress upstream_dns = WiFi.dnsIP();
    if (upstream_dns == IPAddress(0, 0, 0, 0)) {
      upstream_dns = IPAddress(8, 8, 8, 8);
    }

    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
      esp_netif_dns_info_t dns_info;
      dns_info.ip.u_addr.ip4.addr = (uint32_t)upstream_dns;
      dns_info.ip.type = ESP_IPADDR_TYPE_V4;
      esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
      Serial.printf("[ROUTER] AP DHCP DNS set to: %s\n", upstream_dns.toString().c_str());
    }

    napt_active = true;
    return true;
  } else {
    Serial.println(F("[ROUTER] WARNING: ip_napt_enable symbol not available in this core build."));
    napt_active = false;
    return false;
  }
#else
  napt_active = false;
  return false;
#endif
}

// ----------------------------------------------------------------------------
// Build JSON Telemetry String
// ----------------------------------------------------------------------------
static String build_telemetry_json() {
  wifi_sta_list_t sta_list;
  esp_err_t err = esp_wifi_ap_get_sta_list(&sta_list);
  uint8_t connected_count = (err == ESP_OK) ? sta_list.num : 0;

  uint32_t now = millis();

  String json = "{";
  json += "\"connected_count\":" + String(connected_count) + ",";
  json += "\"routing_status\":\"" + String(napt_active ? "active" : "unavailable") + "\",";
  json += "\"ap_ssid\":\"" + String(AP_SSID) + "\",";
  json += "\"ap_ip\":\"" + apIP.toString() + "\",";
  json += "\"sta_ssid\":\"" + String(STA_SSID) + "\",";
  json += "\"sta_status\":\"" + String(sta_connected ? "connected" : "connecting") + "\",";
  json += "\"sta_ip\":\"" + (sta_connected ? WiFi.localIP().toString() : "0.0.0.0") + "\",";
  json += "\"channel\":" + String(WiFi.channel()) + ",";
  json += "\"uptime_secs\":" + String(now / 1000UL) + ",";
  json += "\"devices\":[";

  bool first = true;

  if (err == ESP_OK) {
    for (int i = 0; i < sta_list.num; i++) {
      const uint8_t* sta_mac = sta_list.sta[i].mac;

      portENTER_CRITICAL(&tracker_mux);
      DeviceStats* dev = find_or_create_device(sta_mac, now);

      char id_str[7] = "000000";
      uint32_t up_p = 0, up_b = 0, down_p = 0, down_b = 0;
      uint32_t rate_b = 0, idle_s = 0;
      const char* usage_label = "idle";

      if (dev) {
        strncpy(id_str, dev->anon_id, sizeof(id_str));
        up_p = dev->up_pkts;
        up_b = dev->up_bytes;
        down_p = dev->down_pkts;
        down_b = dev->down_bytes;
        rate_b = dev->recent_rate_bps;
        idle_s = (now >= dev->last_seen_ms) ? (now - dev->last_seen_ms) / 1000UL : 0;
        usage_label = get_usage_label(rate_b, now - dev->last_seen_ms);
      }
      portEXIT_CRITICAL(&tracker_mux);

      if (!first) json += ",";
      first = false;

      json += "{";
      json += "\"id\":\"" + String(id_str) + "\",";
      json += "\"up_pkts\":" + String(up_p) + ",";
      json += "\"up_bytes\":" + String(up_b) + ",";
      json += "\"down_pkts\":" + String(down_p) + ",";
      json += "\"down_bytes\":" + String(down_b) + ",";
      json += "\"rate_bps\":" + String(rate_b) + ",";
      json += "\"usage\":\"" + String(usage_label) + "\",";
      json += "\"idle_secs\":" + String(idle_s);
      json += "}";
    }
  }

  json += "]}";
  return json;
}

// ----------------------------------------------------------------------------
// Telemetry POST to External Web Dashboard
// ----------------------------------------------------------------------------
static void send_telemetry_post() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;

  String url = "http://" + String(DASHBOARD_HOST) + ":" + String(DASHBOARD_PORT) + String(DASHBOARD_PATH);
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(1200); // 1.2s timeout prevents blocking loop()

  String payload = build_telemetry_json();
  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
      // Telemetry received successfully
    } else {
      Serial.printf("[HTTP-POST] Server returned code: %d\n", httpCode);
    }
  } else {
    Serial.printf("[HTTP-POST] Failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}

// ----------------------------------------------------------------------------
// Local Fallback Status API Handler
// ----------------------------------------------------------------------------
static void handle_status_api() {
  String json = build_telemetry_json();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "application/json", json);
}

// ----------------------------------------------------------------------------
// Setup
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1000);
  Serial.println(F("\n=================================================="));
  Serial.println(F("  ESP32 WiFi Router & Occupancy Telemetry Node   "));
  Serial.println(F("=================================================="));

  boot_salt = esp_random();
  Serial.printf("[PRIVACY] Hardware salt generated (0x%08X)\n", boot_salt);

  // 1. AP + Station mode
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);

  // 2. Start SoftAP
  size_t pass_len = strlen(AP_PASS);
  bool ap_ok = false;
  if (pass_len >= 8) {
    ap_ok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  } else {
    Serial.printf("[AP] Note: AP_PASS '%s' is < 8 characters. Starting open AP.\n", AP_PASS);
    ap_ok = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  }

  WiFi.softAPConfig(apIP, apIP, apNetmask);
  WiFi.softAPmacAddress(ap_mac);

  Serial.printf("[AP] Access Point '%s' started on IP %s\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());

  // 3. Start Promiscuous Sniffer for Data Frame Header Analysis
  wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA };
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_rx_callback);
  esp_wifi_set_promiscuous(true);
  Serial.println(F("[SNIFFER] Promiscuous 802.11 data frame header monitor active."));

  // 4. Connect to upstream WiFi
  Serial.printf("[STA] Connecting to upstream WiFi '%s'...\n", STA_SSID);
  WiFi.begin(STA_SSID, STA_PASS);

  // 5. Lightweight local fallback server
  server.on("/api/status", HTTP_GET, handle_status_api);
  server.begin();

  Serial.printf("[TELEMETRY] Configured target: http://%s:%d%s\n",
                DASHBOARD_HOST, DASHBOARD_PORT, DASHBOARD_PATH);
  Serial.println(F("==================================================\n"));
}

// ----------------------------------------------------------------------------
// Main Loop
// ----------------------------------------------------------------------------
void loop() {
  server.handleClient();

  uint32_t now = millis();

  // 1. Maintain 1-second rolling rate buckets
  if (now - last_bucket_tick >= 1000UL) {
    last_bucket_tick = now;
    update_rolling_rates();
  }

  // 2. Transmit telemetry to Web Dashboard every POST_INTERVAL_MS
  if (now - last_post_tick >= POST_INTERVAL_MS) {
    last_post_tick = now;
    send_telemetry_post();
  }

  // 3. Check STA connection status & manage NAPT routing state
  if (WiFi.status() == WL_CONNECTED) {
    if (!sta_connected) {
      sta_connected = true;
      Serial.println(F("\n[STA] Upstream WiFi connected!"));
      Serial.printf("[STA] ESP32 IP: %s\n", WiFi.localIP().toString().c_str());

      // Enable NAPT routing
      init_napt_routing();
    }
  } else {
    if (sta_connected) {
      sta_connected = false;
      napt_active = false;
      Serial.println(F("\n[STA] Upstream WiFi disconnected."));
    }

    // Auto-retry connection every 15 seconds if disconnected
    if (now - sta_last_retry_ms >= 15000UL) {
      sta_last_retry_ms = now;
      if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[STA] Re-trying connection to '%s'...\n", STA_SSID);
        WiFi.begin(STA_SSID, STA_PASS);
      }
    }
  }
}
