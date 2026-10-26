/*
 * ============================================================================
 * Project: ESP32 WiFi Router & Classroom Occupancy Monitor
 * ============================================================================
 * Architecture:
 *  - WiFi.mode(WIFI_AP_STA) + WiFi.softAPConfig() + WiFi.AP.enableNAPT()
 *  - Transparent DNS proxy: intercepts client queries, logs domains, forwards to 8.8.8.8
 *  - Real hardware MAC tracking via esp_wifi_ap_get_sta_list()
 *  - REST API: GET /api/status   — full telemetry JSON
 *              POST /api/toggle  — toggle internet kill-switch
 *              GET/POST /api/config — update dashboard push target
 *  - Optional push to web dashboard (non-blocking, skips gracefully on refusal)
 * ============================================================================
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <esp_wifi.h>

// ----------------------------------------------------------------------------
// Network Configuration
// ----------------------------------------------------------------------------
static const char* ROUTER_SSID = "VoiDX";
static const char* ROUTER_PASS = "sunisgood";

static const char* AP_SSID = "PBL-MC";
static const char* AP_PASS = "12345678";

IPAddress apIP(192, 168, 199, 1);
IPAddress netMask(255, 255, 255, 0);

// ----------------------------------------------------------------------------
// Dashboard Push (best-effort; skipped if client-isolated)
// ----------------------------------------------------------------------------
static String  dashboard_host = "";   // set via /api/config or auto-detected
static int     dashboard_port = 3000;
static const char* DASHBOARD_PATH = "/api/esp32";
#define PUSH_INTERVAL_MS 3000

// ----------------------------------------------------------------------------
// DNS Proxy
// ----------------------------------------------------------------------------
WiFiUDP dnsServer;
WiFiUDP dnsUpstream;
const uint16_t DNS_PORT = 53;
IPAddress publicDns(8, 8, 8, 8);

// ----------------------------------------------------------------------------
// Web Server
// ----------------------------------------------------------------------------
WebServer server(80);

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------
bool     internetAllowed     = true;
bool     naptActive          = false;
uint8_t  lastStationCount    = 255;
uint32_t last_push_ms        = 0;

// ----------------------------------------------------------------------------
// Client Tracking
// ----------------------------------------------------------------------------
const int MAX_CLIENTS = 16;

struct ClientInfo {
  bool     active;
  uint8_t  mac[6];
  char     mac_str[18];
  IPAddress ip;
  char     lastDomain[80];
  uint32_t dnsQueries;
  uint32_t bytesApprox;
  uint32_t lastActiveMs;
};

ClientInfo clients[MAX_CLIENTS];

static void formatMac(const uint8_t* mac, char* out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Sync hardware station list with our tracking array
static void syncStations() {
  wifi_sta_list_t sta_list;
  if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) return;

  unsigned long now = millis();

  // Mark gone clients (not in hardware list for >30s)
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i].active) continue;
    bool found = false;
    for (int j = 0; j < sta_list.num; j++) {
      if (memcmp(clients[i].mac, sta_list.sta[j].mac, 6) == 0) {
        found = true;
        break;
      }
    }
    if (!found && (now - clients[i].lastActiveMs > 30000)) {
      Serial.printf("[STA GONE] %s\n", clients[i].mac_str);
      clients[i].active = false;
    }
  }

  // Add new stations
  for (int j = 0; j < sta_list.num; j++) {
    const uint8_t* mac = sta_list.sta[j].mac;
    bool exists = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (clients[i].active && memcmp(clients[i].mac, mac, 6) == 0) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
          memset(&clients[i], 0, sizeof(clients[i]));
          clients[i].active = true;
          memcpy(clients[i].mac, mac, 6);
          formatMac(mac, clients[i].mac_str);
          clients[i].ip = IPAddress(0, 0, 0, 0);
          strcpy(clients[i].lastDomain, "None");
          clients[i].lastActiveMs = now;
          Serial.printf("[STA JOIN] %s\n", clients[i].mac_str);
          break;
        }
      }
    }
  }
}

// ----------------------------------------------------------------------------
// DNS Proxy
// ----------------------------------------------------------------------------
String parseDnsDomain(const uint8_t* buf, int len) {
  if (len < 13) return "Unknown";
  String domain;
  int i = 12;
  while (i < len && buf[i] != 0) {
    uint8_t labelLen = buf[i];
    if (i + 1 + labelLen > len) break;
    if (domain.length() > 0) domain += '.';
    for (int j = 0; j < labelLen; j++) domain += (char)buf[i + 1 + j];
    i += 1 + labelLen;
  }
  return domain.length() > 0 ? domain : "Unknown";
}

void recordActivity(IPAddress clientIP, const String& domain) {
  unsigned long now = millis();
  // Match existing slot by IP
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active && clients[i].ip == clientIP) {
      strncpy(clients[i].lastDomain, domain.c_str(), sizeof(clients[i].lastDomain) - 1);
      clients[i].dnsQueries++;
      clients[i].bytesApprox += 1500;
      clients[i].lastActiveMs = now;
      return;
    }
  }
  // Assign IP to first unassigned active slot (DHCP not yet seen)
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active && clients[i].ip == IPAddress(0, 0, 0, 0)) {
      clients[i].ip = clientIP;
      strncpy(clients[i].lastDomain, domain.c_str(), sizeof(clients[i].lastDomain) - 1);
      clients[i].dnsQueries++;
      clients[i].bytesApprox += 1500;
      clients[i].lastActiveMs = now;
      return;
    }
  }
}

void processDnsProxy() {
  int pktSize = dnsServer.parsePacket();
  if (pktSize <= 0) return;

  uint8_t buf[512];
  int bytesRead = dnsServer.read(buf, sizeof(buf));
  if (bytesRead <= 0) return;

  IPAddress clientIP   = dnsServer.remoteIP();
  uint16_t clientPort  = dnsServer.remotePort();

  String domain = parseDnsDomain(buf, bytesRead);
  recordActivity(clientIP, domain);

  // Forward to upstream DNS
  dnsUpstream.beginPacket(publicDns, DNS_PORT);
  dnsUpstream.write(buf, bytesRead);
  dnsUpstream.endPacket();

  // Wait up to 200ms for reply
  unsigned long t = millis();
  while (millis() - t < 200) {
    int respSz = dnsUpstream.parsePacket();
    if (respSz > 0) {
      int r = dnsUpstream.read(buf, sizeof(buf));
      if (r > 0) {
        dnsServer.beginPacket(clientIP, clientPort);
        dnsServer.write(buf, r);
        dnsServer.endPacket();
      }
      break;
    }
    delay(2);
  }
}

// ----------------------------------------------------------------------------
// JSON Status Builder
// ----------------------------------------------------------------------------
String buildStatusJson() {
  syncStations();
  unsigned long now = millis();
  uint8_t count = WiFi.softAPgetStationNum();

  String j = "{";
  j += "\"connected_count\":" + String(count) + ",";
  j += "\"connected_clients\":" + String(count) + ",";
  j += "\"internet_allowed\":" + String(internetAllowed ? "true" : "false") + ",";
  j += "\"routing_status\":\"" + String(naptActive ? "active" : "unavailable") + "\",";
  j += "\"ap_ssid\":\"" + String(AP_SSID) + "\",";
  j += "\"ap_ip\":\"" + apIP.toString() + "\",";
  j += "\"sta_ssid\":\"" + String(ROUTER_SSID) + "\",";
  j += "\"sta_status\":\"" + String(WiFi.status() == WL_CONNECTED ? "connected" : "connecting") + "\",";
  j += "\"sta_ip\":\"" + WiFi.localIP().toString() + "\",";
  j += "\"channel\":" + String(WiFi.channel()) + ",";
  j += "\"uptime_secs\":" + String(now / 1000UL) + ",";
  j += "\"devices\":[";

  bool first = true;
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i].active) continue;
    if (!first) j += ",";
    first = false;

    unsigned long idleSec = (now - clients[i].lastActiveMs) / 1000UL;
    const char* usage = idleSec < 10 ? "active" : (idleSec < 45 ? "light" : "idle");

    j += "{";
    j += "\"id\":\"" + String(clients[i].mac_str).substring(9) + "\",";
    j += "\"mac\":\"" + String(clients[i].mac_str) + "\",";
    j += "\"ip\":\"" + clients[i].ip.toString() + "\",";
    j += "\"last_domain\":\"" + String(clients[i].lastDomain) + "\",";
    j += "\"dns_queries\":" + String(clients[i].dnsQueries) + ",";
    j += "\"up_bytes\":" + String(clients[i].bytesApprox / 3) + ",";
    j += "\"down_bytes\":" + String(clients[i].bytesApprox * 2 / 3) + ",";
    j += "\"rate_bps\":0,";
    j += "\"usage\":\"" + String(usage) + "\",";
    j += "\"idle_secs\":" + String(idleSec);
    j += "}";
  }
  j += "]}";
  return j;
}

// ----------------------------------------------------------------------------
// REST API Handlers
// ----------------------------------------------------------------------------
void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleStatus() {
  sendCorsHeaders();
  server.send(200, "application/json", buildStatusJson());
}

void handleToggle() {
  internetAllowed = !internetAllowed;
  // Update NAPT immediately
  bool uplinkOk = (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != IPAddress(0, 0, 0, 0));
  bool targetState = uplinkOk && internetAllowed;
  if (targetState != naptActive) {
    naptActive = targetState;
    WiFi.AP.enableNAPT(naptActive);
  }
  Serial.printf("[CTRL] Internet: %s\n", internetAllowed ? "ENABLED" : "BLOCKED");
  sendCorsHeaders();
  server.send(200, "application/json", buildStatusJson());
}

void handleConfig() {
  if (server.hasArg("host")) {
    dashboard_host = server.arg("host");
    Serial.printf("[CFG] Push host → %s\n", dashboard_host.c_str());
  }
  if (server.hasArg("port")) {
    dashboard_port = server.arg("port").toInt();
  }
  sendCorsHeaders();
  String j = "{\"status\":\"ok\",\"host\":\"" + dashboard_host + "\",\"port\":" + String(dashboard_port) + "}";
  server.send(200, "application/json", j);
}

void handleOptions() {
  sendCorsHeaders();
  server.send(204);
}

// ----------------------------------------------------------------------------
// Best-effort push to dashboard (non-blocking with short timeout)
// ----------------------------------------------------------------------------
void pushTelemetry() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (dashboard_host.length() == 0) return;  // no target configured, skip

  WiFiClient httpClient;
  HTTPClient http;
  String url = "http://" + dashboard_host + ":" + String(dashboard_port) + DASHBOARD_PATH;
  if (!http.begin(httpClient, url)) return;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  http.setTimeout(800);  // short timeout; if AP isolation blocks it, we just move on

  String payload = buildStatusJson();
  int code = http.POST(payload);
  http.end();

  if (code > 0) {
    Serial.printf("[PUSH] → %s HTTP %d\n", dashboard_host.c_str(), code);
  }
  // Silent on failure — web dashboard polls ESP32 directly as primary mechanism
}

// ----------------------------------------------------------------------------
// Setup
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n==================================================");
  Serial.println("  ESP32 WiFi NAT Router & Classroom Monitor");
  Serial.println("==================================================");

  memset(clients, 0, sizeof(clients));

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);  // max TX for reliable routing

  // Start SoftAP first (proven config from feature/wifi-nat-setup)
  WiFi.softAPConfig(apIP, apIP, netMask);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[AP] SSID: %s | GW: %s | Pass: %s\n", AP_SSID, apIP.toString().c_str(), AP_PASS);

  // DNS proxy on port 53 for all AP clients
  dnsServer.begin(DNS_PORT);
  Serial.println("[DNS] Transparent proxy active on port 53");

  // REST API
  server.enableCORS(true);
  server.on("/api/status",  HTTP_GET,    handleStatus);
  server.on("/api/status",  HTTP_OPTIONS, handleOptions);
  server.on("/api/toggle",  HTTP_POST,   handleToggle);
  server.on("/api/toggle",  HTTP_GET,    handleToggle);
  server.on("/api/toggle",  HTTP_OPTIONS, handleOptions);
  server.on("/api/config",  HTTP_POST,   handleConfig);
  server.on("/api/config",  HTTP_GET,    handleConfig);
  server.begin();
  Serial.println("[HTTP] REST API started on port 80");

  // Connect to upstream WiFi
  WiFi.begin(ROUTER_SSID, ROUTER_PASS);
  Serial.printf("[STA] Connecting to '%s'...\n", ROUTER_SSID);
}

// ----------------------------------------------------------------------------
// Loop
// ----------------------------------------------------------------------------
void loop() {
  server.handleClient();

  bool uplinkOk = (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != IPAddress(0, 0, 0, 0));
  bool targetNapt = uplinkOk && internetAllowed;

  if (targetNapt != naptActive) {
    naptActive = targetNapt;
    WiFi.AP.enableNAPT(naptActive);
    if (naptActive) {
      String ip = WiFi.localIP().toString();
      Serial.printf(">>> NAT ACTIVE — AP clients have internet via %s <<<\n", ip.c_str());
      // Auto-configure push target if not already set
      if (dashboard_host.length() == 0) {
        // Try to push to the default gateway of the uplink (usually the router)
        // The web dashboard will reach us via our STA IP, so leave host empty until
        // someone calls /api/config or the dashboard's POST arrives
        Serial.printf("[INFO] Dashboard host not set. Reach me at: http://%s/api/status\n", ip.c_str());
      }
    } else {
      Serial.println(">>> NAT PAUSED — Internet blocked <<<");
    }
  }

  // DNS proxy (only needed when routing is active, but also handle when STA is connecting
  // so clients can still get 'no internet' gracefully rather than hanging on DNS)
  processDnsProxy();

  // Log station count changes
  uint8_t cnt = WiFi.softAPgetStationNum();
  if (cnt != lastStationCount) {
    lastStationCount = cnt;
    Serial.printf("[AP] Connected stations: %d\n", cnt);
    syncStations();
  }

  // Best-effort push to web dashboard
  unsigned long now = millis();
  if (now - last_push_ms >= PUSH_INTERVAL_MS) {
    last_push_ms = now;
    if (uplinkOk) pushTelemetry();
  }

  delay(2);
}
