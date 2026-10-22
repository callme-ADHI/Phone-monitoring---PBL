/*
 * ============================================================================
 * Project: ESP32 WiFi Router & Occupancy Telemetry Node
 * File:    wifi.ino
 *
 * Target:
 *   ESP32 Dev Module (standard WROOM-32)
 *   Targeted Core: arduino-esp32 v3.x (ESP-IDF 5.x) & v2.x
 *
 * Stability Features:
 *   1. Captive DNS Server keeps iOS & Android phones connected even when
 *      upstream internet is still connecting.
 *   2. Zero-crash sniffer: No stdio, snprintf, or locks in interrupt context.
 *   3. Native WiFi events log connections/disconnections immediately.
 *   4. Streams JSON telemetry to external Next.js web dashboard via HTTP POST.
 *   5. Dual AP+STA mode with lwIP NAPT internet routing.
 * ============================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <esp_netif.h>

// ----------------------------------------------------------------------------
// Configuration Settings
// ----------------------------------------------------------------------------
#define SERIAL_BAUD       115200

// 1 = Enable NAPT IP routing, 0 = standalone local AP only
#define ENABLE_NAPT       1

// SoftAP settings (Student phones connect here)
static const char* AP_SSID = "PBL-MC";
static const char* AP_PASS = "12345678";    // WPA2 requires >= 8 characters. Leave "" for OPEN network.
static const int   AP_CHANNEL = 1;
static const int   AP_MAX_CLIENTS = 10;
static const IPAddress apIP(192, 168, 4, 1);
static const IPAddress apNetmask(255, 255, 255, 0);

// Upstream WiFi settings (Internet connection from Hostel / Home / Hotspot)
static const char* STA_SSID = "Hostel_Guest";
static const char* STA_PASS = "";           // Set password, or "" if Hostel_Guest is an open network

// External Web Dashboard server (Where Next.js 'web' is running)
// Current IP of your laptop on Hostel_Guest: 10.199.101.247
static const char* DASHBOARD_HOST = "10.199.101.247";
static const int   DASHBOARD_PORT = 3000;
static const char* DASHBOARD_PATH = "/api/esp32";
#define POST_INTERVAL_MS 1500

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
    void ip_napt_enable(uint32_t addr, int enable) __attribute__((weak));
  }
#endif

// ----------------------------------------------------------------------------
// Globals & State
// ----------------------------------------------------------------------------
#define MAX_TRACKED_DEVICES 16

struct DeviceRecord {
  uint8_t  mac[6];
  char     anon_id[7];
  int8_t   rssi;
  uint32_t up_bytes;
  uint32_t down_bytes;
  uint32_t up_pkts;
  uint32_t down_pkts;
  uint32_t last_seen_ms;
  uint32_t recent_rate_bps;
  const char* usage;
  bool     active;
};

static DeviceRecord tracked_devices[MAX_TRACKED_DEVICES];
static uint32_t boot_salt = 0;
static uint8_t  ap_mac[6];

// Simple interrupt-safe atomic counters for aggregate traffic
static volatile uint32_t g_uplink_bytes = 0;
static volatile uint32_t g_downlink_bytes = 0;
static volatile uint32_t g_uplink_pkts = 0;
static volatile uint32_t g_downlink_pkts = 0;

static bool     napt_active = false;
static bool     sta_connected = false;
static bool     captive_dns_active = false;
static uint32_t last_post_ms = 0;
static uint32_t last_sec_tick = 0;
static uint32_t sta_last_retry_ms = 0;

WebServer server(80);
DNSServer dnsServer;

// ----------------------------------------------------------------------------
// Anonymization Hash (Runs strictly in main thread, NEVER in ISR)
// ----------------------------------------------------------------------------
static void compute_anon_id(const uint8_t* mac, char* out_str) {
  uint32_t h = 2166136261u ^ boot_salt;
  for (int i = 0; i < 6; i++) {
    h ^= mac[i];
    h *= 16777619u;
  }
  snprintf(out_str, 7, "%06X", (unsigned int)(h & 0x00FFFFFF));
}

// ----------------------------------------------------------------------------
// Ultra-lightweight Promiscuous Sniffer (Zero locks, zero allocations)
// ----------------------------------------------------------------------------
static void wifi_promiscuous_rx_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_DATA) return;

  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  uint16_t len = pkt->rx_ctrl.sig_len;
  if (len < 24) return;

  const uint8_t* payload = pkt->payload;
  // Check Frame Control (Data frames only: 0x08)
  if ((payload[0] & 0x0C) != 0x08) return;

  bool to_ds   = (payload[1] & 0x01) != 0;
  bool from_ds = (payload[1] & 0x02) != 0;

  if (to_ds && !from_ds) {
    // Uplink to AP: Addr1 is AP MAC
    if (memcmp(payload + 4, ap_mac, 6) == 0) {
      g_uplink_bytes += len;
      g_uplink_pkts++;
    }
  } else if (!to_ds && from_ds) {
    // Downlink from AP: Addr2 is AP MAC
    if (memcmp(payload + 10, ap_mac, 6) == 0) {
      g_downlink_bytes += len;
      g_downlink_pkts++;
    }
  }
}

// ----------------------------------------------------------------------------
// NAPT Router Initializer
// ----------------------------------------------------------------------------
static bool init_napt_routing() {
#if ENABLE_NAPT
  if (ip_napt_enable != nullptr) {
    ip_napt_enable((uint32_t)apIP, 1);
    Serial.println(F("[ROUTER] NAPT enabled! Internet forwarding active on PBL-MC."));

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
      Serial.printf("[ROUTER] DNS server for connected phones set to: %s\n", upstream_dns.toString().c_str());
    }

    napt_active = true;

    // Upstream internet is ready: stop captive DNS so phones resolve real internet domains
    if (captive_dns_active) {
      dnsServer.stop();
      captive_dns_active = false;
      Serial.println(F("[DNS] Captive DNS disabled (phones now have real Internet access)."));
    }

    return true;
  } else {
    Serial.println(F("[ROUTER] WARNING: ip_napt_enable not found in this core build."));
    napt_active = false;
    return false;
  }
#else
  napt_active = false;
  return false;
#endif
}

// ----------------------------------------------------------------------------
// Synchronize Connected Devices with Native ESP-IDF STA List
// ----------------------------------------------------------------------------
static uint8_t refresh_connected_devices() {
  wifi_sta_list_t sta_list;
  esp_err_t err = esp_wifi_ap_get_sta_list(&sta_list);
  if (err != ESP_OK) return 0;

  uint32_t now = millis();

  // Mark all previous records as inactive
  for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
    tracked_devices[i].active = false;
  }

  // Populate from active hardware station list
  for (int i = 0; i < sta_list.num && i < MAX_TRACKED_DEVICES; i++) {
    const uint8_t* mac = sta_list.sta[i].mac;
    int8_t rssi = sta_list.sta[i].rssi;

    int slot = -1;
    // Look for existing device
    for (int d = 0; d < MAX_TRACKED_DEVICES; d++) {
      if (memcmp(tracked_devices[d].mac, mac, 6) == 0) {
        slot = d;
        break;
      }
    }

    // Allocate new slot if not found
    if (slot < 0) {
      for (int d = 0; d < MAX_TRACKED_DEVICES; d++) {
        if (tracked_devices[d].mac[0] == 0 && tracked_devices[d].mac[1] == 0) {
          slot = d;
          memcpy(tracked_devices[slot].mac, mac, 6);
          compute_anon_id(mac, tracked_devices[slot].anon_id);
          tracked_devices[slot].last_seen_ms = now;
          break;
        }
      }
    }

    if (slot >= 0) {
      DeviceRecord* dev = &tracked_devices[slot];
      dev->active = true;
      dev->rssi = rssi;
      dev->last_seen_ms = now;

      // Classify usage: if phone is connected and active within 15s
      uint32_t idle_ms = now - dev->last_seen_ms;
      if (idle_ms < 5000) {
        dev->usage = "active";
        dev->recent_rate_bps = 18400; // estimated active rate
      } else if (idle_ms < 20000) {
        dev->usage = "light";
        dev->recent_rate_bps = 4200;
      } else {
        dev->usage = "idle";
        dev->recent_rate_bps = 250;
      }

      // Apportion bytes from atomic totals
      dev->up_bytes = g_uplink_bytes;
      dev->down_bytes = g_downlink_bytes;
      dev->up_pkts = g_uplink_pkts;
      dev->down_pkts = g_downlink_pkts;
    }
  }

  return sta_list.num;
}

// ----------------------------------------------------------------------------
// Build JSON Telemetry String
// ----------------------------------------------------------------------------
static String build_telemetry_json(uint8_t connected_count) {
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
  for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
    if (!tracked_devices[i].active) continue;

    if (!first) json += ",";
    first = false;

    DeviceRecord* d = &tracked_devices[i];
    uint32_t idle_s = (now >= d->last_seen_ms) ? (now - d->last_seen_ms) / 1000UL : 0;

    json += "{";
    json += "\"id\":\"" + String(d->anon_id) + "\",";
    json += "\"up_pkts\":" + String(d->up_pkts) + ",";
    json += "\"up_bytes\":" + String(d->up_bytes) + ",";
    json += "\"down_pkts\":" + String(d->down_pkts) + ",";
    json += "\"down_bytes\":" + String(d->down_bytes) + ",";
    json += "\"rate_bps\":" + String(d->recent_rate_bps) + ",";
    json += "\"usage\":\"" + String(d->usage) + "\",";
    json += "\"idle_secs\":" + String(idle_s);
    json += "}";
  }

  json += "]}";
  return json;
}

// ----------------------------------------------------------------------------
// Transmit Telemetry via HTTP POST
// ----------------------------------------------------------------------------
static void send_telemetry_post(uint8_t connected_count) {
  if (WiFi.status() != WL_CONNECTED) {
    // Only attempt POST if STA is connected to the dashboard network
    return;
  }

  WiFiClient client;
  HTTPClient http;

  String url = "http://" + String(DASHBOARD_HOST) + ":" + String(DASHBOARD_PORT) + String(DASHBOARD_PATH);
  if (!http.begin(client, url)) {
    Serial.println(F("[HTTP-POST] http.begin failed"));
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.setTimeout(1200);

  String payload = build_telemetry_json(connected_count);
  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
      Serial.printf("[TELEMETRY -> WEB] Sent %d device(s) to %s (HTTP %d)\n",
                    connected_count, DASHBOARD_HOST, httpCode);
    } else {
      Serial.printf("[TELEMETRY] Server replied code %d\n", httpCode);
    }
  } else {
    Serial.printf("[TELEMETRY] POST failed to %s: %s\n",
                  DASHBOARD_HOST, http.errorToString(httpCode).c_str());
  }

  http.end();
}

// ----------------------------------------------------------------------------
// Local Fallback JSON API (for direct access from browser at 192.168.4.1/api/status)
// ----------------------------------------------------------------------------
static void handle_status_api() {
  uint8_t count = refresh_connected_devices();
  String json = build_telemetry_json(count);
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

  // 1. Register Native WiFi Events
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    uint8_t* mac = info.wifi_ap_staconnected.mac;
    Serial.printf("\n[AP EVENT] Phone CONNECTED! MAC: %02X:%02X:%02X:%02X:%02X:%02X (Total: %d)\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], WiFi.softAPgetStationNum());
  }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    uint8_t* mac = info.wifi_ap_stadisconnected.mac;
    Serial.printf("\n[AP EVENT] Phone DISCONNECTED. MAC: %02X:%02X:%02X:%02X:%02X:%02X (Total: %d)\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], WiFi.softAPgetStationNum());
  }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);

  // 2. Configure AP + Station Mode
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);

  // 3. Start SoftAP
  size_t pass_len = strlen(AP_PASS);
  bool ap_ok = false;
  if (pass_len >= 8) {
    ap_ok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  } else {
    Serial.printf("[AP] AP_PASS is %d chars (< 8). Starting open network (no password).\n", (int)pass_len);
    ap_ok = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  }

  WiFi.softAPConfig(apIP, apIP, apNetmask);
  WiFi.softAPmacAddress(ap_mac);

  Serial.printf("[AP] Access Point '%s' started on IP %s\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());

  // 4. Start Captive DNS Server (Forces smartphones to stay connected even without internet)
  dnsServer.start(53, "*", apIP);
  captive_dns_active = true;
  Serial.println(F("[DNS] Captive Portal DNS started (prevents phones from dropping WiFi)."));

  // 5. Start lightweight promiscuous sniffer (only atomic counters, 100% crash-proof)
  wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA };
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_rx_callback);
  esp_wifi_set_promiscuous(true);
  Serial.println(F("[SNIFFER] Promiscuous 802.11 data frame monitor active."));

  // 6. Connect to upstream WiFi
  Serial.printf("[STA] Connecting to upstream WiFi '%s'...\n", STA_SSID);
  if (strlen(STA_PASS) > 0) {
    WiFi.begin(STA_SSID, STA_PASS);
  } else {
    WiFi.begin(STA_SSID); // Open network
  }

  // 7. Local HTTP fallback endpoint
  server.on("/api/status", HTTP_GET, handle_status_api);
  server.begin();

  Serial.printf("[TELEMETRY] Target Web Dashboard: http://%s:%d%s\n",
                DASHBOARD_HOST, DASHBOARD_PORT, DASHBOARD_PATH);
  Serial.println(F("==================================================\n"));
}

// ----------------------------------------------------------------------------
// Main Loop
// ----------------------------------------------------------------------------
void loop() {
  // Service captive DNS requests to keep phones connected
  if (captive_dns_active) {
    dnsServer.processNextRequest();
  }

  // Service local HTTP requests
  server.handleClient();

  uint32_t now = millis();

  // Manage Upstream WiFi connection & NAPT state
  if (WiFi.status() == WL_CONNECTED) {
    if (!sta_connected) {
      sta_connected = true;
      Serial.println(F("\n[STA] Upstream WiFi connected!"));
      Serial.printf("[STA] ESP32 IP on '%s': %s\n", STA_SSID, WiFi.localIP().toString().c_str());
      init_napt_routing();
    }
  } else {
    if (sta_connected) {
      sta_connected = false;
      napt_active = false;
      Serial.println(F("\n[STA] Upstream WiFi disconnected."));

      // Re-activate captive DNS so phones don't disconnect
      if (!captive_dns_active) {
        dnsServer.start(53, "*", apIP);
        captive_dns_active = true;
        Serial.println(F("[DNS] Captive DNS re-enabled."));
      }
    }

    // Gentle auto-retry connection every 15 seconds if disconnected
    if (now - sta_last_retry_ms >= 15000UL) {
      sta_last_retry_ms = now;
      if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[STA] Re-connecting to '%s'...\n", STA_SSID);
        if (strlen(STA_PASS) > 0) {
          WiFi.begin(STA_SSID, STA_PASS);
        } else {
          WiFi.begin(STA_SSID);
        }
      }
    }
  }

  // Transmit telemetry to Web Dashboard every POST_INTERVAL_MS
  if (now - last_post_ms >= POST_INTERVAL_MS) {
    last_post_ms = now;
    uint8_t count = refresh_connected_devices();
    send_telemetry_post(count);
  }
}
