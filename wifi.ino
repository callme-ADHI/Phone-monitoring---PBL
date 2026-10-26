/*
 * ============================================================================
 * Project: ESP32 WiFi NAT Router & Classroom Monitor
 * ============================================================================
 * Traffic counting:
 *   - Promiscuous mode sniffer on AP interface captures real 802.11 DATA frames
 *   - Per-client actual byte counters for TX (upload) and RX (download)
 *   - RSSI from esp_wifi_ap_get_sta_list() — hardware value
 *   - DNS interception gives real query count and last requested domain
 *   - rate_bps calculated from bytes delta over time window
 * ============================================================================
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>

// ----------------------------------------------------------------------------
// Network Configuration
// ----------------------------------------------------------------------------
static const char* ROUTER_SSID = "VoiDX";
static const char* ROUTER_PASS = "sunisgood";
static const char* AP_SSID    = "PBL-MC";
static const char* AP_PASS    = "12345678";

IPAddress apIP(192, 168, 199, 1);
IPAddress netMask(255, 255, 255, 0);

// Dashboard push target (optional best-effort)
static String  dashboard_host = "";
static int     dashboard_port = 3000;
static const char* DASHBOARD_PATH = "/api/esp32";
#define PUSH_INTERVAL_MS 2000

// ----------------------------------------------------------------------------
// DNS Proxy
// ----------------------------------------------------------------------------
WiFiUDP dnsServer;
WiFiUDP dnsUpstream;
const uint16_t DNS_PORT = 53;
IPAddress publicDns(8, 8, 8, 8);

WebServer server(80);

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------
bool    internetAllowed  = true;
bool    naptActive       = false;
uint8_t lastStationCount = 255;
uint32_t last_push_ms    = 0;

// ----------------------------------------------------------------------------
// Per-client tracking
// ----------------------------------------------------------------------------
const int MAX_CLIENTS = 16;

struct ClientInfo {
  bool      active;
  uint8_t   mac[6];
  char      mac_str[18];
  IPAddress ip;
  int8_t    rssi;              // real RSSI from hardware
  char      lastDomain[80];
  uint32_t  dnsQueries;
  // Real traffic bytes (from promiscuous sniffer)
  uint32_t  txBytes;           // data sent FROM this client (upload)
  uint32_t  rxBytes;           // data sent TO this client (download)
  uint32_t  txPkts;
  uint32_t  rxPkts;
  // Rate calculation
  uint32_t  lastRateTxBytes;
  uint32_t  lastRateRxBytes;
  uint32_t  lastRateMs;
  uint32_t  rateBps;           // real calculated bps
  uint32_t  lastActiveMs;
};

ClientInfo clients[MAX_CLIENTS];
portMUX_TYPE clientMux = portMUX_INITIALIZER_UNLOCKED;

// ----------------------------------------------------------------------------
// Helper: Format MAC
// ----------------------------------------------------------------------------
static void formatMac(const uint8_t* mac, char* out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int findClientByMac(const uint8_t* mac) {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active && memcmp(clients[i].mac, mac, 6) == 0)
      return i;
  }
  return -1;
}

static int findOrCreateClient(const uint8_t* mac) {
  int idx = findClientByMac(mac);
  if (idx >= 0) return idx;
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i].active) {
      memset(&clients[i], 0, sizeof(clients[i]));
      clients[i].active = true;
      memcpy(clients[i].mac, mac, 6);
      formatMac(mac, clients[i].mac_str);
      clients[i].ip = IPAddress(0, 0, 0, 0);
      clients[i].rssi = -99;
      strcpy(clients[i].lastDomain, "None");
      clients[i].lastActiveMs = millis();
      clients[i].lastRateMs = millis();
      Serial.printf("[STA JOIN] %s\n", clients[i].mac_str);
      return i;
    }
  }
  return -1; // table full
}

// ----------------------------------------------------------------------------
// 802.11 Promiscuous Sniffer — counts real per-client bytes
// Frame layout: https://en.wikipedia.org/wiki/802.11_frame_types
// ----------------------------------------------------------------------------
typedef struct {
  uint16_t frame_ctrl;
  uint16_t duration;
  uint8_t  addr1[6]; // Destination (receiver)
  uint8_t  addr2[6]; // Source (transmitter)
  uint8_t  addr3[6]; // BSSID or other
} wifi_ieee80211_mac_hdr_t;

static void IRAM_ATTR promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_DATA) return;

  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  wifi_ieee80211_mac_hdr_t* hdr = (wifi_ieee80211_mac_hdr_t*)pkt->payload;
  uint16_t fc = hdr->frame_ctrl;

  // Frame control bits: ToDS (bit 8), FromDS (bit 9)
  bool toDS   = (fc >> 8) & 0x01;
  bool fromDS = (fc >> 8) & 0x02;

  // AP SoftAP BSSID — our AP MAC
  // ToDS=1, FromDS=0 → client uploading to AP (addr2 = client MAC)
  // ToDS=0, FromDS=1 → AP sending to client (addr1 = client MAC)

  uint16_t dataLen = pkt->rx_ctrl.sig_len;
  if (dataLen < 24) return; // minimum 802.11 MAC header

  portENTER_CRITICAL_ISR(&clientMux);

  if (toDS && !fromDS) {
    // Upload: client (addr2) → AP
    int idx = findClientByMac(hdr->addr2);
    if (idx >= 0) {
      clients[idx].txBytes += dataLen;
      clients[idx].txPkts++;
      clients[idx].lastActiveMs = millis();
    }
  } else if (!toDS && fromDS) {
    // Download: AP → client (addr1)
    int idx = findClientByMac(hdr->addr1);
    if (idx >= 0) {
      clients[idx].rxBytes += dataLen;
      clients[idx].rxPkts++;
    }
  }

  portEXIT_CRITICAL_ISR(&clientMux);
}

// ----------------------------------------------------------------------------
// Sync hardware station list + RSSI
// ----------------------------------------------------------------------------
static void syncStations() {
  wifi_sta_list_t sta_list;
  if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) return;

  unsigned long now = millis();

  // Mark gone
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i].active) continue;
    bool found = false;
    for (int j = 0; j < sta_list.num; j++) {
      if (memcmp(clients[i].mac, sta_list.sta[j].mac, 6) == 0) {
        found = true;
        clients[i].rssi = sta_list.sta[j].rssi; // real RSSI
        break;
      }
    }
    if (!found && (now - clients[i].lastActiveMs > 30000)) {
      Serial.printf("[STA GONE] %s\n", clients[i].mac_str);
      clients[i].active = false;
    }
  }

  // Add new
  for (int j = 0; j < sta_list.num; j++) {
    const uint8_t* mac = sta_list.sta[j].mac;
    int idx = findOrCreateClient(mac);
    if (idx >= 0) {
      clients[idx].rssi = sta_list.sta[j].rssi;
    }
  }
}

// ----------------------------------------------------------------------------
// Update per-client rate_bps (called every ~1s)
// ----------------------------------------------------------------------------
static void updateRates() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i].active) continue;
    uint32_t dt = now - clients[i].lastRateMs;
    if (dt >= 1000) {
      uint32_t bytesDelta = (clients[i].txBytes + clients[i].rxBytes)
                          - (clients[i].lastRateTxBytes + clients[i].lastRateRxBytes);
      clients[i].rateBps = (bytesDelta * 8 * 1000) / dt; // bits per second
      clients[i].lastRateTxBytes = clients[i].txBytes;
      clients[i].lastRateRxBytes = clients[i].rxBytes;
      clients[i].lastRateMs = now;
    }
  }
}

// ----------------------------------------------------------------------------
// DNS Proxy — real DNS packet interception
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

void recordDnsActivity(IPAddress clientIP, const String& domain) {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active && clients[i].ip == clientIP) {
      strncpy(clients[i].lastDomain, domain.c_str(), sizeof(clients[i].lastDomain) - 1);
      clients[i].dnsQueries++;
      clients[i].lastActiveMs = now;
      return;
    }
  }
  // Assign IP to first slot with unassigned IP
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active && clients[i].ip == IPAddress(0, 0, 0, 0)) {
      clients[i].ip = clientIP;
      strncpy(clients[i].lastDomain, domain.c_str(), sizeof(clients[i].lastDomain) - 1);
      clients[i].dnsQueries++;
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

  IPAddress clientIP  = dnsServer.remoteIP();
  uint16_t clientPort = dnsServer.remotePort();

  String domain = parseDnsDomain(buf, bytesRead);
  recordDnsActivity(clientIP, domain);

  // Forward to upstream DNS
  dnsUpstream.beginPacket(publicDns, DNS_PORT);
  dnsUpstream.write(buf, bytesRead);
  dnsUpstream.endPacket();

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
// Build Status JSON
// ----------------------------------------------------------------------------
String buildStatusJson() {
  syncStations();
  updateRates();
  unsigned long now = millis();
  uint8_t count = WiFi.softAPgetStationNum();

  String j = "{";
  j += "\"connected_count\":" + String(count) + ",";
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
    const char* usage = idleSec < 8 ? "active" : (idleSec < 30 ? "light" : "idle");

    j += "{";
    j += "\"id\":\"" + String(clients[i].mac_str).substring(9) + "\",";
    j += "\"mac\":\"" + String(clients[i].mac_str) + "\",";
    j += "\"ip\":\"" + clients[i].ip.toString() + "\",";
    j += "\"rssi\":" + String(clients[i].rssi) + ",";
    j += "\"last_domain\":\"" + String(clients[i].lastDomain) + "\",";
    j += "\"dns_queries\":" + String(clients[i].dnsQueries) + ",";
    // Real traffic from promiscuous sniffer
    j += "\"up_bytes\":" + String(clients[i].txBytes) + ",";
    j += "\"down_bytes\":" + String(clients[i].rxBytes) + ",";
    j += "\"up_pkts\":" + String(clients[i].txPkts) + ",";
    j += "\"down_pkts\":" + String(clients[i].rxPkts) + ",";
    j += "\"rate_bps\":" + String(clients[i].rateBps) + ",";
    j += "\"usage\":\"" + String(usage) + "\",";
    j += "\"idle_secs\":" + String(idleSec);
    j += "}";
  }
  j += "]}";
  return j;
}

// ----------------------------------------------------------------------------
// REST API
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
  if (server.hasArg("host")) dashboard_host = server.arg("host");
  if (server.hasArg("port")) dashboard_port = server.arg("port").toInt();
  sendCorsHeaders();
  String j = "{\"status\":\"ok\",\"host\":\"" + dashboard_host + "\",\"port\":" + String(dashboard_port) + "}";
  server.send(200, "application/json", j);
}

void handleOptions() {
  sendCorsHeaders();
  server.send(204);
}

// ----------------------------------------------------------------------------
// Best-effort push (non-blocking)
// ----------------------------------------------------------------------------
void pushTelemetry() {
  if (WiFi.status() != WL_CONNECTED || dashboard_host.length() == 0) return;
  WiFiClient httpClient;
  HTTPClient http;
  String url = "http://" + dashboard_host + ":" + String(dashboard_port) + DASHBOARD_PATH;
  if (!http.begin(httpClient, url)) return;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  http.setTimeout(600);
  http.POST(buildStatusJson());
  http.end();
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
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  WiFi.softAPConfig(apIP, apIP, netMask);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[AP] SSID: %s | GW: %s\n", AP_SSID, apIP.toString().c_str());

  // Enable promiscuous mode for real traffic counting
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
  Serial.println("[SNIFFER] Promiscuous traffic counters active");

  dnsServer.begin(DNS_PORT);
  Serial.println("[DNS] Transparent proxy on port 53");

  server.enableCORS(true);
  server.on("/api/status", HTTP_GET,     handleStatus);
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/toggle", HTTP_POST,    handleToggle);
  server.on("/api/toggle", HTTP_GET,     handleToggle);
  server.on("/api/toggle", HTTP_OPTIONS, handleOptions);
  server.on("/api/config", HTTP_POST,    handleConfig);
  server.on("/api/config", HTTP_GET,     handleConfig);
  server.begin();
  Serial.println("[HTTP] REST API on port 80");

  WiFi.begin(ROUTER_SSID, ROUTER_PASS);
  Serial.printf("[STA] Connecting to '%s'...\n", ROUTER_SSID);
}

// ----------------------------------------------------------------------------
// Loop
// ----------------------------------------------------------------------------
static uint32_t lastRateUpdate = 0;

void loop() {
  server.handleClient();

  bool uplinkOk  = (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != IPAddress(0, 0, 0, 0));
  bool targetNapt = uplinkOk && internetAllowed;

  if (targetNapt != naptActive) {
    naptActive = targetNapt;
    WiFi.AP.enableNAPT(naptActive);
    if (naptActive) {
      Serial.printf(">>> NAT ACTIVE — %s <<<\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println(">>> NAT PAUSED <<<");
    }
  }

  processDnsProxy();

  uint8_t cnt = WiFi.softAPgetStationNum();
  if (cnt != lastStationCount) {
    lastStationCount = cnt;
    Serial.printf("[AP] Stations: %d\n", cnt);
    syncStations();
  }

  // Update rate counters every second
  uint32_t now = millis();
  if (now - lastRateUpdate >= 1000) {
    lastRateUpdate = now;
    updateRates();
  }

  if (now - last_push_ms >= PUSH_INTERVAL_MS) {
    last_push_ms = now;
    if (uplinkOk) pushTelemetry();
  }

  delay(1);
}
