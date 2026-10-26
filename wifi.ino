/*
 * ============================================================================
 * Project: ESP32 WiFi Router & Classroom Occupancy Monitor
 * File:    wifi.ino
 *
 * Architecture based on feature/wifi-nat-setup:
 *   1. WiFi.mode(WIFI_AP_STA) with WiFi.softAPConfig(apIP, apIP, netMask)
 *   2. WiFi.softAP(AP_SSID, AP_PASS)
 *   3. Dynamic NAPT: WiFi.AP.enableNAPT(targetNaptState)
 *   4. DNS Proxy with tracking on port 53 (direct Internet domain resolution)
 *   5. Real MAC address tracking via WiFi events / station list
 *   6. CORS-enabled REST API: /api/status, /api/toggle, /api/config
 * ============================================================================
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <esp_wifi.h>

// ----------------------------------------------------------------------------
// Configuration Settings
// ----------------------------------------------------------------------------
static const char* ROUTER_SSID = "VoiDX";
static const char* ROUTER_PASS = "sunisgood";

static const char* AP_SSID     = "PBL-MC";
static const char* AP_PASS     = "12345678";

IPAddress apIP(192, 168, 199, 1);
IPAddress netMask(255, 255, 255, 0);

// Web Dashboard push target (optional; web dashboard also polls ESP32 automatically)
static String target_dashboard_host = "10.38.17.190";
static int    target_dashboard_port = 3000;
static const char* DASHBOARD_PATH   = "/api/esp32";
#define POST_INTERVAL_MS 2000

WebServer server(80);
WiFiUDP dnsServer;
WiFiUDP dnsUpstream;
const uint16_t DNS_PORT = 53;
IPAddress publicDns(8, 8, 8, 8);

bool internetAllowed     = true;
bool naptActive          = false;
uint8_t lastStationCount = 255;
uint32_t last_post_ms    = 0;

// ----------------------------------------------------------------------------
// Client Activity & MAC Tracking State
// ----------------------------------------------------------------------------
const int MAX_CLIENTS = 16;

struct ClientActivity {
  IPAddress ip;
  uint8_t   mac[6];
  char      mac_str[18];
  char      lastDomain[64];
  unsigned long lastActiveMs;
  uint32_t  queryCount;
  uint32_t  bytesApprox;
  bool      active;
};

ClientActivity trackedClients[MAX_CLIENTS];

// ----------------------------------------------------------------------------
// Helper: Format MAC Address
// ----------------------------------------------------------------------------
static void format_mac(const uint8_t* mac, char* out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ----------------------------------------------------------------------------
// Synchronize Connected Stations (Hardware MACs)
// ----------------------------------------------------------------------------
static void sync_stations() {
  wifi_sta_list_t sta_list;
  if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) return;

  unsigned long now = millis();

  // Mark absent clients
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!trackedClients[i].active) continue;
    bool stillHere = false;
    for (int j = 0; j < sta_list.num; j++) {
      if (memcmp(trackedClients[i].mac, sta_list.sta[j].mac, 6) == 0) {
        stillHere = true;
        break;
      }
    }
    if (!stillHere && (now - trackedClients[i].lastActiveMs > 60000)) {
      trackedClients[i].active = false;
    }
  }

  // Register newly connected hardware stations
  for (int j = 0; j < sta_list.num; j++) {
    const uint8_t* mac = sta_list.sta[j].mac;
    bool found = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (trackedClients[i].active && memcmp(trackedClients[i].mac, mac, 6) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!trackedClients[i].active) {
          trackedClients[i].active = true;
          memcpy(trackedClients[i].mac, mac, 6);
          format_mac(mac, trackedClients[i].mac_str);
          trackedClients[i].ip = IPAddress(0, 0, 0, 0);
          strcpy(trackedClients[i].lastDomain, "None");
          trackedClients[i].lastActiveMs = now;
          trackedClients[i].queryCount = 0;
          trackedClients[i].bytesApprox = 4096;
          break;
        }
      }
    }
  }
}

// ----------------------------------------------------------------------------
// DNS Packet Parsing & Activity Tracking
// ----------------------------------------------------------------------------
String parseDnsDomain(const uint8_t* buffer, int length) {
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

void recordDeviceActivity(IPAddress clientIP, const String& domain) {
  unsigned long now = millis();

  // Match by IP
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (trackedClients[i].active && trackedClients[i].ip == clientIP) {
      strncpy(trackedClients[i].lastDomain, domain.c_str(), sizeof(trackedClients[i].lastDomain) - 1);
      trackedClients[i].lastDomain[sizeof(trackedClients[i].lastDomain) - 1] = '\0';
      trackedClients[i].lastActiveMs = now;
      trackedClients[i].queryCount++;
      trackedClients[i].bytesApprox += 1500;
      return;
    }
  }

  // If IP was 0.0.0.0, associate IP with first unassigned active station
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (trackedClients[i].active && trackedClients[i].ip == IPAddress(0, 0, 0, 0)) {
      trackedClients[i].ip = clientIP;
      strncpy(trackedClients[i].lastDomain, domain.c_str(), sizeof(trackedClients[i].lastDomain) - 1);
      trackedClients[i].lastActiveMs = now;
      trackedClients[i].queryCount++;
      trackedClients[i].bytesApprox += 1500;
      return;
    }
  }
}

void processDnsProxy() {
  int packetSize = dnsServer.parsePacket();
  if (packetSize > 0) {
    uint8_t buffer[512];
    int bytesRead = dnsServer.read(buffer, sizeof(buffer));
    IPAddress clientIP = dnsServer.remoteIP();
    uint16_t clientPort = dnsServer.remotePort();

    if (bytesRead > 0) {
      String domain = parseDnsDomain(buffer, bytesRead);
      recordDeviceActivity(clientIP, domain);

      // Forward to public upstream DNS
      dnsUpstream.beginPacket(publicDns, DNS_PORT);
      dnsUpstream.write(buffer, bytesRead);
      dnsUpstream.endPacket();

      unsigned long startTime = millis();
      while (millis() - startTime < 150) {
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
// JSON Builder
// ----------------------------------------------------------------------------
String buildStatusJson() {
  sync_stations();
  unsigned long now = millis();
  uint8_t clientCount = WiFi.softAPgetStationNum();

  String json = "{";
  json += "\"connected_count\":" + String(clientCount) + ",";
  json += "\"connected_clients\":" + String(clientCount) + ",";
  json += "\"internet_allowed\":" + String(internetAllowed ? "true" : "false") + ",";
  json += "\"routing_status\":\"" + String(naptActive ? "active" : "unavailable") + "\",";
  json += "\"ap_ssid\":\"" + String(AP_SSID) + "\",";
  json += "\"ap_ip\":\"" + apIP.toString() + "\",";
  json += "\"sta_ssid\":\"" + String(ROUTER_SSID) + "\",";
  json += "\"sta_status\":\"" + String((WiFi.status() == WL_CONNECTED) ? "connected" : "connecting") + "\",";
  json += "\"sta_ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"channel\":" + String(WiFi.channel()) + ",";
  json += "\"uptime_secs\":" + String(now / 1000UL) + ",";
  json += "\"devices\":[";

  bool first = true;
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!trackedClients[i].active) continue;

    if (!first) json += ",";
    first = false;

    unsigned long secAgo = (now - trackedClients[i].lastActiveMs) / 1000UL;
    const char* usage = (secAgo < 10) ? "active" : (secAgo < 30 ? "light" : "idle");

    json += "{";
    json += "\"id\":\"" + String(trackedClients[i].mac_str).substring(9) + "\",";
    json += "\"mac\":\"" + String(trackedClients[i].mac_str) + "\",";
    json += "\"ip\":\"" + trackedClients[i].ip.toString() + "\",";
    json += "\"last_domain\":\"" + String(trackedClients[i].lastDomain) + "\",";
    json += "\"dns_queries\":" + String(trackedClients[i].queryCount) + ",";
    json += "\"queries\":" + String(trackedClients[i].queryCount) + ",";
    json += "\"up_bytes\":" + String(trackedClients[i].bytesApprox / 3) + ",";
    json += "\"down_bytes\":" + String(trackedClients[i].bytesApprox * 2 / 3) + ",";
    json += "\"rate_bps\":1024,";
    json += "\"usage\":\"" + String(usage) + "\",";
    json += "\"idle_secs\":" + String(secAgo) + ",";
    json += "\"last_active_sec\":" + String(secAgo);
    json += "}";
  }
  json += "]}";
  return json;
}

// ----------------------------------------------------------------------------
// REST API Handlers
// ----------------------------------------------------------------------------
void handleApiStatus() {
  server.send(200, "application/json", buildStatusJson());
}

void handleApiToggle() {
  internetAllowed = !internetAllowed;
  Serial.printf("[CONTROL] Internet toggled via API to: %s\n", internetAllowed ? "ENABLED" : "DISABLED");
  handleApiStatus();
}

void handleApiConfig() {
  if (server.hasArg("host")) {
    target_dashboard_host = server.arg("host");
    Serial.printf("[CONFIG] Target host updated to: %s\n", target_dashboard_host.c_str());
  }
  if (server.hasArg("port")) {
    target_dashboard_port = server.arg("port").toInt();
  }
  String json = "{\"status\":\"ok\",\"host\":\"" + target_dashboard_host + "\",\"port\":" + String(target_dashboard_port) + "}";
  server.send(200, "application/json", json);
}

// Optional Push to Web Dashboard
void pushTelemetry() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;

  String url = "http://" + target_dashboard_host + ":" + String(target_dashboard_port) + String(DASHBOARD_PATH);
  if (!http.begin(client, url)) return;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  http.setTimeout(1200);

  String payload = buildStatusJson();
  int httpCode = http.POST(payload);
  http.end();
}

// ----------------------------------------------------------------------------
// Setup
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- ESP32 NAT Router & Monitor Booting ---");

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  // SoftAP Configuration from working branch
  WiFi.softAPConfig(apIP, apIP, netMask);
  WiFi.softAP(AP_SSID, AP_PASS);

  dnsServer.begin(DNS_PORT);

  // Configure CORS so local browsers can query directly
  server.enableCORS(true);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/toggle", HTTP_POST, handleApiToggle);
  server.on("/api/toggle", HTTP_GET, handleApiToggle);
  server.on("/api/config", HTTP_POST, handleApiConfig);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.begin();

  Serial.printf("[AP] Gateway IP: %s | SSID: %s\n", apIP.toString().c_str(), AP_SSID);

  // Connect to Upstream Wi-Fi
  WiFi.begin(ROUTER_SSID, ROUTER_PASS);
  Serial.printf("[STA] Connecting to upstream WiFi '%s'...\n", ROUTER_SSID);
}

// ----------------------------------------------------------------------------
// Main Loop
// ----------------------------------------------------------------------------
void loop() {
  server.handleClient();

  bool uplinkConnected = (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != IPAddress(0, 0, 0, 0));
  bool targetNaptState = uplinkConnected && internetAllowed;

  if (targetNaptState != naptActive) {
    naptActive = targetNaptState;
    WiFi.AP.enableNAPT(naptActive);
    if (naptActive) {
      Serial.printf(">>> NAT ROUTER ACTIVE - Uplink IP: %s <<<\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println(">>> NAT ROUTER PAUSED - Internet Blocked <<<");
    }
  }

  if (naptActive) {
    processDnsProxy();
  }

  uint8_t currentStationCount = WiFi.softAPgetStationNum();
  if (currentStationCount != lastStationCount) {
    lastStationCount = currentStationCount;
    Serial.printf("[STATIONS] Active connected devices: %d\n", currentStationCount);
  }

  // Periodic Telemetry Push
  unsigned long now = millis();
  if (now - last_post_ms >= POST_INTERVAL_MS) {
    last_post_ms = now;
    pushTelemetry();
  }

  delay(3);
}
