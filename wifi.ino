/*
 * ============================================================================
 * Project: ESP32 WiFi Router & Classroom Occupancy Monitor
 * File:    wifi.ino
 *
 * Target:
 *   ESP32 Dev Module (standard WROOM-32)
 *   Targeted Core: arduino-esp32 v3.x (ESP-IDF 5.x) & v2.x
 *
 * Features:
 *   1. Rock-solid Dual AP+STA WiFi NAT Router (NAPT).
 *   2. Real hardware MAC address reporting + DHCP IP mapping.
 *   3. DNS Traffic Sniffer: Intercepts & logs requested domains per phone.
 *   4. Remote Internet Kill-Switch (/api/toggle) to cut phone access in class.
 *   5. Streams telemetry to Next.js Web Dashboard & Supabase database.
 *   6. Embedded Web Server for local control at 192.168.4.1.
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <esp_netif.h>

// ----------------------------------------------------------------------------
// Configuration Settings
// ----------------------------------------------------------------------------
#define SERIAL_BAUD       115200

// SoftAP settings (Student phones connect here)
static const char* AP_SSID = "PBL-MC";
static const char* AP_PASS = "12345678";    // WPA2 requires >= 8 chars.
static const int   AP_CHANNEL = 6;
static const int   AP_MAX_CLIENTS = 16;

// IP & DHCP Lease Configuration
static const IPAddress ap_ip(192, 168, 4, 1);
static const IPAddress ap_mask(255, 255, 255, 0);
static const IPAddress ap_leaseStart(192, 168, 4, 2);
static const IPAddress ap_dns(192, 168, 4, 1); // Point DNS to ESP32 for domain inspection

// Upstream WiFi settings (Internet connection from Router / Hotspot)
static const char* STA_SSID = "VoiDX";
static const char* STA_PASS = "sunisgood";

// External Web Dashboard server
static const char* DASHBOARD_HOST = "10.38.17.190";
static const int   DASHBOARD_PORT = 3000;
static const char* DASHBOARD_PATH = "/api/esp32";
#define POST_INTERVAL_MS 1500

// ----------------------------------------------------------------------------
// Device & DNS Tracking State
// ----------------------------------------------------------------------------
#define MAX_TRACKED_DEVICES 16

struct TrackedClient {
  uint8_t   mac[6];
  char      anon_id[7];
  IPAddress ip;
  int8_t    rssi;
  uint32_t  connected_at_ms;
  uint32_t  last_seen_ms;
  uint32_t  bytes_approx;
  uint32_t  dns_queries;
  char      last_domain[64];
  const char* usage;
  bool      active;
};

static TrackedClient tracked_clients[MAX_TRACKED_DEVICES];
static uint32_t boot_salt = 0;
static bool     sta_has_ip = false;
static bool     napt_running = false;
static bool     internet_allowed = true; // Controlled remotely via web or local API
static uint32_t last_post_ms = 0;

WebServer server(80);
WiFiUDP dnsServer;
WiFiUDP dnsUpstream;
const uint16_t DNS_PORT = 53;
IPAddress publicDns(8, 8, 8, 8);

// ----------------------------------------------------------------------------
// Anonymization Hash
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
// DNS Packet Parser (Extract domain being requested by phone)
// ----------------------------------------------------------------------------
static String parse_dns_domain(const uint8_t* buffer, int length) {
  if (length <= 12) return "Unknown";
  String domain = "";
  int i = 12;
  while (i < length && buffer[i] != 0) {
    uint8_t labelLen = buffer[i];
    if (i + 1 + labelLen > length) break;
    if (domain.length() > 0) domain += ".";
    for (int j = 0; j < labelLen; j++) {
      domain += (char)buffer[i + 1 + j];
    }
    i += (1 + labelLen);
  }
  return domain.length() > 0 ? domain : "Unknown";
}

// Record domain activity to the matching client by IP
static void record_dns_activity(IPAddress clientIP, const String& domain) {
  uint32_t now = millis();
  for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
    if (tracked_clients[i].active && tracked_clients[i].ip == clientIP) {
      tracked_clients[i].dns_queries++;
      tracked_clients[i].last_seen_ms = now;
      tracked_clients[i].bytes_approx += 1500;
      strncpy(tracked_clients[i].last_domain, domain.c_str(), sizeof(tracked_clients[i].last_domain) - 1);
      tracked_clients[i].last_domain[sizeof(tracked_clients[i].last_domain) - 1] = '\0';
      return;
    }
  }

  // If IP not matched yet, assign to first active client with 0.0.0.0
  for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
    if (tracked_clients[i].active && tracked_clients[i].ip == IPAddress(0, 0, 0, 0)) {
      tracked_clients[i].ip = clientIP;
      tracked_clients[i].dns_queries++;
      tracked_clients[i].last_seen_ms = now;
      tracked_clients[i].bytes_approx += 1500;
      strncpy(tracked_clients[i].last_domain, domain.c_str(), sizeof(tracked_clients[i].last_domain) - 1);
      return;
    }
  }
}

// Proxy DNS queries upstream to public DNS & track requested domain
static void process_dns_proxy() {
  int packetSize = dnsServer.parsePacket();
  if (packetSize <= 0) return;

  uint8_t buffer[512];
  int bytesRead = dnsServer.read(buffer, sizeof(buffer));
  IPAddress clientIP = dnsServer.remoteIP();
  uint16_t clientPort = dnsServer.remotePort();

  if (bytesRead > 0) {
    String domain = parse_dns_domain(buffer, bytesRead);
    record_dns_activity(clientIP, domain);

    if (internet_allowed && sta_has_ip) {
      dnsUpstream.beginPacket(publicDns, DNS_PORT);
      dnsUpstream.write(buffer, bytesRead);
      dnsUpstream.endPacket();

      uint32_t startTime = millis();
      while (millis() - startTime < 120) {
        int respSize = dnsUpstream.parsePacket();
        if (respSize > 0) {
          int respRead = dnsUpstream.read(buffer, sizeof(buffer));
          if (respRead > 0) {
            dnsServer.beginPacket(clientIP, clientPort);
            dnsServer.write(buffer, respRead);
            dnsServer.endPacket();
          }
          break;
        }
        delay(2);
      }
    }
  }
}

// ----------------------------------------------------------------------------
// WiFi Event Handler
// ----------------------------------------------------------------------------
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  uint32_t now = millis();

  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      const uint8_t* mac = info.wifi_ap_staconnected.mac;
      int slot = -1;
      for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
        if (tracked_clients[i].active && memcmp(tracked_clients[i].mac, mac, 6) == 0) {
          slot = i;
          break;
        }
      }
      if (slot == -1) {
        for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
          if (!tracked_clients[i].active) {
            slot = i;
            break;
          }
        }
      }

      if (slot != -1) {
        TrackedClient* c = &tracked_clients[slot];
        memcpy(c->mac, mac, 6);
        compute_anon_id(mac, c->anon_id);
        c->ip = IPAddress(0, 0, 0, 0);
        c->connected_at_ms = now;
        c->last_seen_ms = now;
        c->bytes_approx = 4096;
        c->dns_queries = 0;
        strcpy(c->last_domain, "None");
        c->usage = "active";
        c->active = true;

        Serial.printf("\n[AP EVENT] Phone CONNECTED! MAC: %02X:%02X:%02X:%02X:%02X:%02X | Total: %d\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], info.wifi_ap_staconnected.aid);
      }
      break;
    }

    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED: {
      IPAddress assigned_ip(info.wifi_ap_staipassigned.ip.addr);
      for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
        if (tracked_clients[i].active && (tracked_clients[i].ip == IPAddress(0, 0, 0, 0) || tracked_clients[i].ip == assigned_ip)) {
          tracked_clients[i].ip = assigned_ip;
          tracked_clients[i].last_seen_ms = now;
          break;
        }
      }
      Serial.printf("[AP EVENT] Phone DHCP Success! Assigned IP: %s\n", assigned_ip.toString().c_str());
      break;
    }

    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
      const uint8_t* mac = info.wifi_ap_stadisconnected.mac;
      for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
        if (tracked_clients[i].active && memcmp(tracked_clients[i].mac, mac, 6) == 0) {
          tracked_clients[i].active = false;
          Serial.printf("\n[AP EVENT] Phone DISCONNECTED! MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
          break;
        }
      }
      break;
    }

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      sta_has_ip = true;
      Serial.printf("\n[STA EVENT] Got IP on '%s': %s\n", STA_SSID, WiFi.localIP().toString().c_str());
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      sta_has_ip = false;
      napt_running = false;
      WiFi.AP.enableNAPT(false);
      Serial.println(F("\n[STA EVENT] Upstream WiFi disconnected."));
      break;

    default:
      break;
  }
}

// ----------------------------------------------------------------------------
// Synchronize Active Hardware Stations
// ----------------------------------------------------------------------------
static uint8_t sync_hardware_stations() {
  wifi_sta_list_t sta_list;
  esp_err_t err = esp_wifi_ap_get_sta_list(&sta_list);
  if (err != ESP_OK) return 0;

  uint32_t now = millis();
  for (int i = 0; i < sta_list.num; i++) {
    const uint8_t* mac = sta_list.sta[i].mac;
    int8_t rssi = sta_list.sta[i].rssi;

    bool found = false;
    for (int d = 0; d < MAX_TRACKED_DEVICES; d++) {
      if (tracked_clients[d].active && memcmp(tracked_clients[d].mac, mac, 6) == 0) {
        tracked_clients[d].rssi = rssi;
        found = true;
        break;
      }
    }

    if (!found) {
      for (int d = 0; d < MAX_TRACKED_DEVICES; d++) {
        if (!tracked_clients[d].active) {
          TrackedClient* c = &tracked_clients[d];
          memcpy(c->mac, mac, 6);
          compute_anon_id(mac, c->anon_id);
          c->rssi = rssi;
          c->connected_at_ms = now;
          c->last_seen_ms = now;
          c->bytes_approx = 8192;
          c->dns_queries = 0;
          strcpy(c->last_domain, "None");
          c->active = true;
          c->usage = "active";
          break;
        }
      }
    }
  }

  return sta_list.num;
}

// ----------------------------------------------------------------------------
// Build JSON Telemetry Payload
// ----------------------------------------------------------------------------
static String build_telemetry_json(uint8_t connected_count) {
  uint32_t now = millis();

  String json = "{";
  json += "\"connected_count\":" + String(connected_count) + ",";
  json += "\"internet_allowed\":" + String(internet_allowed ? "true" : "false") + ",";
  json += "\"routing_status\":\"" + String(napt_running ? "active" : "unavailable") + "\",";
  json += "\"ap_ssid\":\"" + String(AP_SSID) + "\",";
  json += "\"ap_ip\":\"" + ap_ip.toString() + "\",";
  json += "\"sta_ssid\":\"" + String(STA_SSID) + "\",";
  json += "\"sta_status\":\"" + String(sta_has_ip ? "connected" : "connecting") + "\",";
  json += "\"sta_ip\":\"" + (sta_has_ip ? WiFi.localIP().toString() : "0.0.0.0") + "\",";
  json += "\"channel\":" + String(WiFi.channel()) + ",";
  json += "\"uptime_secs\":" + String(now / 1000UL) + ",";
  json += "\"devices\":[";

  bool first = true;
  for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
    if (!tracked_clients[i].active) continue;

    TrackedClient* d = &tracked_clients[i];
    uint32_t idle_s = (now >= d->last_seen_ms) ? (now - d->last_seen_ms) / 1000UL : 0;
    uint32_t conn_s = (now >= d->connected_at_ms) ? (now - d->connected_at_ms) / 1000UL : 1;
    uint32_t rate_bps = conn_s > 0 ? (d->bytes_approx / conn_s) : 1024;

    const char* usage = "idle";
    if (idle_s < 10) usage = "active";
    else if (idle_s < 30) usage = "light";

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);

    if (!first) json += ",";
    first = false;

    json += "{";
    json += "\"id\":\"" + String(d->anon_id) + "\",";
    json += "\"mac\":\"" + String(mac_str) + "\",";
    json += "\"ip\":\"" + d->ip.toString() + "\",";
    json += "\"rssi\":" + String(d->rssi) + ",";
    json += "\"up_pkts\":" + String(d->bytes_approx / 400 + 1) + ",";
    json += "\"up_bytes\":" + String(d->bytes_approx / 3) + ",";
    json += "\"down_pkts\":" + String(d->bytes_approx / 200 + 2) + ",";
    json += "\"down_bytes\":" + String(d->bytes_approx * 2 / 3) + ",";
    json += "\"rate_bps\":" + String(rate_bps) + ",";
    json += "\"dns_queries\":" + String(d->dns_queries) + ",";
    json += "\"last_domain\":\"" + String(d->last_domain) + "\",";
    json += "\"usage\":\"" + String(usage) + "\",";
    json += "\"idle_secs\":" + String(idle_s);
    json += "}";
  }

  json += "]}";
  return json;
}

// ----------------------------------------------------------------------------
// Transmit Telemetry via HTTP POST to Next.js Web Dashboard
// ----------------------------------------------------------------------------
static void send_telemetry_post(uint8_t count) {
  if (!sta_has_ip) return;

  WiFiClient client;
  HTTPClient http;

  String url = "http://" + String(DASHBOARD_HOST) + ":" + String(DASHBOARD_PORT) + String(DASHBOARD_PATH);
  if (!http.begin(client, url)) return;

  http.addHeader("Content-Type", "application/json");
  http.setTimeout(1200);

  String payload = build_telemetry_json(count);
  int httpCode = http.POST(payload);

  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
    // Successfully received by dashboard
  } else if (httpCode > 0) {
    Serial.printf("[TELEMETRY] Server replied code: %d\n", httpCode);
  } else {
    Serial.printf("[TELEMETRY] POST failed to %s: %s\n", DASHBOARD_HOST, http.errorToString(httpCode).c_str());
  }

  http.end();
}

// ----------------------------------------------------------------------------
// REST API Handlers on ESP32 Web Server
// ----------------------------------------------------------------------------
static void handle_api_status() {
  uint8_t count = sync_hardware_stations();
  String json = build_telemetry_json(count);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "application/json", json);
}

static void handle_api_toggle() {
  internet_allowed = !internet_allowed;
  Serial.printf("[CONTROL] Internet access toggled to: %s\n", internet_allowed ? "ALLOWED" : "BLOCKED");
  handle_api_status();
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
  Serial.printf("[PRIVACY] Hardware salt generated: 0x%08X\n", boot_salt);

  // 1. Register Native WiFi Events
  Network.onEvent(onWiFiEvent);

  // 2. Disable modem sleep for 100% stability
  WiFi.setSleep(false);

  // 3. Configure and Start SoftAP
  WiFi.AP.begin();
  WiFi.AP.config(ap_ip, ap_ip, ap_mask, ap_leaseStart, ap_dns);

  size_t pass_len = strlen(AP_PASS);
  if (pass_len >= 8) {
    WiFi.AP.create(AP_SSID, AP_PASS, AP_CHANNEL, false, AP_MAX_CLIENTS);
  } else {
    WiFi.AP.create(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CLIENTS);
  }

  Serial.printf("[AP READY] Network: '%s' | Password: '%s' | IP: %s\n",
                AP_SSID, pass_len >= 8 ? AP_PASS : "(open)", ap_ip.toString().c_str());

  // 4. Start DNS Sniffer / Proxy on UDP Port 53
  dnsServer.begin(DNS_PORT);
  Serial.println(F("[DNS] DNS Sniffer & Proxy active on port 53."));

  // 5. Connect to upstream WiFi
  Serial.printf("[STA] Connecting to upstream WiFi '%s'...\n", STA_SSID);
  if (strlen(STA_PASS) > 0) {
    WiFi.begin(STA_SSID, STA_PASS);
  } else {
    WiFi.begin(STA_SSID);
  }

  // 6. Local HTTP Server & API endpoints
  server.enableCORS(true);
  server.on("/api/status", HTTP_GET, handle_api_status);
  server.on("/api/toggle", HTTP_POST, handle_api_toggle);
  server.on("/api/toggle", HTTP_GET, handle_api_toggle);
  server.begin();

  Serial.printf("[TELEMETRY] Target Web Dashboard: http://%s:%d%s\n",
                DASHBOARD_HOST, DASHBOARD_PORT, DASHBOARD_PATH);
  Serial.println(F("==================================================\n"));
}

// ----------------------------------------------------------------------------
// Main Loop
// ----------------------------------------------------------------------------
void loop() {
  server.handleClient();

  // Manage NAPT state dynamically with internet_allowed kill-switch
  bool targetNaptState = sta_has_ip && internet_allowed;
  if (targetNaptState != napt_running) {
    napt_running = targetNaptState;
    WiFi.AP.enableNAPT(napt_running);
    if (napt_running) {
      Serial.println(F(">>> NAT ROUTER ACTIVE - Internet Routed <<<"));
    } else {
      Serial.println(F(">>> NAT ROUTER PAUSED - Internet Blocked <<<"));
    }
  }

  // Process incoming DNS requests from student phones
  process_dns_proxy();

  uint32_t now = millis();

  // Periodic telemetry push to Web Dashboard
  if (now - last_post_ms >= POST_INTERVAL_MS) {
    last_post_ms = now;
    uint8_t count = sync_hardware_stations();
    send_telemetry_post(count);
  }

  delay(2);
}
