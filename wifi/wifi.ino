/*
 * ============================================================================
 * Project: ESP32 WiFi NAT Router, Classroom Policy Monitor & STM32 Bridge
 * Target : ESP32 (NodeMCU / DOIT DevKit V1)
 * 
 * Hardware Connections to STM32 Blue Pill:
 * ----------------------------------------------------------------------------
 *   ESP32 GPIO17 (TX2) ---> STM32 PA10 (USART1 RX)
 *   ESP32 GPIO16 (RX2) <--- STM32 PA9  (USART1 TX) [Optional]
 *   ESP32 GND          <---> STM32 GND              [Mandatory Common Ground!]
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
IPAddress apLeaseStart(192, 168, 199, 2);
IPAddress publicDns(8, 8, 8, 8);

static String  dashboard_host = "";
static int     dashboard_port = 3000;
static const char* DASHBOARD_PATH = "/api/esp32";
#define PUSH_INTERVAL_MS 2000

// ----------------------------------------------------------------------------
// STM32 Bridge (Hardware Serial2 on GPIO16-RX, GPIO17-TX)
// ----------------------------------------------------------------------------
#define STM32_RX_PIN 16
#define STM32_TX_PIN 17
HardwareSerial STMSerial(2);

// ----------------------------------------------------------------------------
// Classroom Policy & Camera State
// ----------------------------------------------------------------------------
int  latestPersonCount = 0;
bool classInSession    = true;
bool anyHighUsage      = false;
String schedStartTime  = "08:00";
String schedEndTime    = "18:00";
unsigned long lastStmBroadcastMs = 0;

#define HIGH_USAGE_RATE_THRESHOLD  200000 // bits/sec (25 KB/s)
#define HIGH_USAGE_BYTE_THRESHOLD  300000 // bytes total

WiFiUDP dnsServer;
WiFiUDP dnsUpstream;
const uint16_t DNS_PORT = 53;

WebServer server(80);

bool    internetAllowed  = true;
bool    naptActive       = false;
uint8_t lastStationCount = 255;
uint32_t last_push_ms    = 0;

const int MAX_CLIENTS = 16;

struct ClientInfo {
  bool      active;
  uint8_t   mac[6];
  char      mac_str[18];
  IPAddress ip;
  int8_t    rssi;
  char      lastDomain[80];
  uint32_t  dnsQueries;
  uint32_t  txBytes;
  uint32_t  rxBytes;
  uint32_t  txPkts;
  uint32_t  rxPkts;
  uint32_t  lastRateTxBytes;
  uint32_t  lastRateRxBytes;
  uint32_t  lastRateMs;
  uint32_t  rateBps;
  uint32_t  lastActiveMs;
};

ClientInfo clients[MAX_CLIENTS];
portMUX_TYPE clientMux = portMUX_INITIALIZER_UNLOCKED;

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
  return -1;
}

typedef struct {
  uint16_t frame_ctrl;
  uint16_t duration;
  uint8_t  addr1[6];
  uint8_t  addr2[6];
  uint8_t  addr3[6];
} wifi_ieee80211_mac_hdr_t;

static void IRAM_ATTR promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_DATA) return;

  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  wifi_ieee80211_mac_hdr_t* hdr = (wifi_ieee80211_mac_hdr_t*)pkt->payload;
  uint16_t fc = hdr->frame_ctrl;

  bool toDS   = (fc >> 8) & 0x01;
  bool fromDS = (fc >> 8) & 0x02;

  uint16_t dataLen = pkt->rx_ctrl.sig_len;
  if (dataLen < 24) return;

  portENTER_CRITICAL_ISR(&clientMux);

  if (toDS && !fromDS) {
    int idx = findClientByMac(hdr->addr2);
    if (idx >= 0) {
      clients[idx].txBytes += dataLen;
      clients[idx].txPkts++;
      clients[idx].lastActiveMs = millis();
    }
  } else if (!toDS && fromDS) {
    int idx = findClientByMac(hdr->addr1);
    if (idx >= 0) {
      clients[idx].rxBytes += dataLen;
      clients[idx].rxPkts++;
    }
  }

  portEXIT_CRITICAL_ISR(&clientMux);
}

static void syncStations() {
  wifi_sta_list_t sta_list;
  if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) return;

  unsigned long now = millis();

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i].active) continue;
    bool found = false;
    for (int j = 0; j < sta_list.num; j++) {
      if (memcmp(clients[i].mac, sta_list.sta[j].mac, 6) == 0) {
        found = true;
        clients[i].rssi = sta_list.sta[j].rssi;
        break;
      }
    }
    if (!found && (now - clients[i].lastActiveMs > 30000)) {
      Serial.printf("[STA GONE] %s\n", clients[i].mac_str);
      clients[i].active = false;
    }
  }

  for (int j = 0; j < sta_list.num; j++) {
    const uint8_t* mac = sta_list.sta[j].mac;
    int idx = findOrCreateClient(mac);
    if (idx >= 0) {
      clients[idx].rssi = sta_list.sta[j].rssi;
    }
  }
}

static void updateRates() {
  unsigned long now = millis();
  anyHighUsage = false;

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i].active) continue;
    uint32_t dt = now - clients[i].lastRateMs;
    if (dt >= 1000) {
      uint32_t bytesDelta = (clients[i].txBytes + clients[i].rxBytes)
                          - (clients[i].lastRateTxBytes + clients[i].lastRateRxBytes);
      clients[i].rateBps = (bytesDelta * 8 * 1000) / dt;
      clients[i].lastRateTxBytes = clients[i].txBytes;
      clients[i].lastRateRxBytes = clients[i].rxBytes;
      clients[i].lastRateMs = now;
    }

    if (clients[i].rateBps > HIGH_USAGE_RATE_THRESHOLD ||
        (clients[i].txBytes + clients[i].rxBytes) > HIGH_USAGE_BYTE_THRESHOLD) {
      anyHighUsage = true;
    }
  }
}

void broadcastToSTM32() {
  uint8_t phoneCount = WiFi.softAPgetStationNum();

  char telemMsg[128];
  snprintf(telemMsg, sizeof(telemMsg),
           "$TELEM,P:%d,M:%d,S:%d,H:%d,T1:%s,T2:%s*\n",
           latestPersonCount,
           phoneCount,
           classInSession ? 1 : 0,
           anyHighUsage ? 1 : 0,
           schedStartTime.c_str(),
           schedEndTime.c_str());

  STMSerial.print(telemMsg);
  static uint8_t logCounter = 0;
  if (++logCounter % 5 == 0) {
    Serial.printf("[STM32 TX] %s", telemMsg);
  }
}

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

String buildStatusJson() {
  syncStations();
  updateRates();
  unsigned long now = millis();
  uint8_t count = WiFi.softAPgetStationNum();

  String j = "{";
  j += "\"connected_count\":" + String(count) + ",";
  j += "\"person_count\":" + String(latestPersonCount) + ",";
  j += "\"high_usage\":" + String(anyHighUsage ? "true" : "false") + ",";
  j += "\"class_in_session\":" + String(classInSession ? "true" : "false") + ",";
  j += "\"start_time\":\"" + schedStartTime + "\",";
  j += "\"end_time\":\"" + schedEndTime + "\",";
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

void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
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

void handlePersonCount() {
  if (server.hasArg("count")) {
    latestPersonCount = server.arg("count").toInt();
  } else if (server.hasArg("plain")) {
    String body = server.arg("plain");
    int idx = body.indexOf("\"personCount\":");
    if (idx != -1) {
      latestPersonCount = body.substring(idx + 14).toInt();
    } else {
      idx = body.indexOf("\"count\":");
      if (idx != -1) {
        latestPersonCount = body.substring(idx + 8).toInt();
      }
    }
  }
  Serial.printf("[CAM->STM32] Person count received: %d\n", latestPersonCount);
  broadcastToSTM32();
  sendCorsHeaders();
  server.send(200, "application/json", "{\"status\":\"ok\",\"person_count\":" + String(latestPersonCount) + "}");
}

void handleSchedule() {
  if (server.hasArg("start_time")) schedStartTime = server.arg("start_time");
  if (server.hasArg("end_time"))   schedEndTime   = server.arg("end_time");
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    int i1 = body.indexOf("\"start_time\":");
    if (i1 != -1) {
      int q1 = body.indexOf("\"", i1 + 13);
      int q2 = body.indexOf("\"", q1 + 1);
      if (q1 != -1 && q2 != -1) schedStartTime = body.substring(q1 + 1, q2);
    }
    int i2 = body.indexOf("\"end_time\":");
    if (i2 != -1) {
      int q1 = body.indexOf("\"", i2 + 11);
      int q2 = body.indexOf("\"", q1 + 1);
      if (q1 != -1 && q2 != -1) schedEndTime = body.substring(q1 + 1, q2);
    }
  }
  Serial.printf("[SCHED] Timings set to %s - %s\n", schedStartTime.c_str(), schedEndTime.c_str());
  broadcastToSTM32();
  sendCorsHeaders();
  server.send(200, "application/json", "{\"status\":\"ok\",\"start\":\"" + schedStartTime + "\",\"end\":\"" + schedEndTime + "\"}");
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

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n==================================================");
  Serial.println("  ESP32 WiFi NAT Router, Policy Node & STM32 Bridge");
  Serial.println("==================================================");

  STMSerial.begin(115200, SERIAL_8N1, STM32_RX_PIN, STM32_TX_PIN);
  Serial.printf("[STM32 LINK] Serial2 initialized on TX2: GPIO%d, RX2: GPIO%d @ 115200\n", STM32_TX_PIN, STM32_RX_PIN);

  memset(clients, 0, sizeof(clients));

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  WiFi.softAPConfig(apIP, apIP, netMask, apLeaseStart, publicDns);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[AP] SSID: %s | GW: %s | DNS: %s\n", AP_SSID, apIP.toString().c_str(), publicDns.toString().c_str());

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
  Serial.println("[SNIFFER] Promiscuous traffic counters active");

  dnsServer.begin(DNS_PORT);
  Serial.println("[DNS] Transparent proxy on port 53");

  server.enableCORS(true);
  server.on("/api/status",   HTTP_GET,     handleStatus);
  server.on("/api/status",   HTTP_OPTIONS, handleOptions);
  server.on("/api/toggle",   HTTP_POST,    handleToggle);
  server.on("/api/toggle",   HTTP_GET,     handleToggle);
  server.on("/api/toggle",   HTTP_OPTIONS, handleOptions);
  server.on("/api/person",   HTTP_POST,    handlePersonCount);
  server.on("/api/person",   HTTP_GET,     handlePersonCount);
  server.on("/api/schedule", HTTP_POST,    handleSchedule);
  server.on("/api/schedule", HTTP_GET,     handleSchedule);
  server.on("/api/config",   HTTP_POST,    handleConfig);
  server.on("/api/config",   HTTP_GET,     handleConfig);
  server.begin();
  Serial.println("[HTTP] REST API on port 80 ready for Web & Camera");

  WiFi.begin(ROUTER_SSID, ROUTER_PASS);
  Serial.printf("[STA] Connecting to '%s'...\n", ROUTER_SSID);
}

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
    broadcastToSTM32();
  }

  uint32_t now = millis();
  if (now - lastRateUpdate >= 1000) {
    lastRateUpdate = now;
    updateRates();
  }

  if (now - lastStmBroadcastMs >= 800) {
    lastStmBroadcastMs = now;
    broadcastToSTM32();
  }


  // Auto-reconnect to upstream WiFi if connection dropped
  static unsigned long lastStaCheck = 0;
  if (now - lastStaCheck > 5000) {
    lastStaCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[STA] Reconnecting to upstream WiFi...");
      WiFi.reconnect();
    }
  }

  if (now - last_push_ms >= PUSH_INTERVAL_MS) {
    last_push_ms = now;
    if (uplinkOk) pushTelemetry();
  }

  delay(1);
}
