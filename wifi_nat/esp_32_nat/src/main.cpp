#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>

const char* ROUTER_SSID = "Nowifi";
const char* ROUTER_PASS = "whatpassword"; // Replace with your actual password

const char* AP_SSID     = "ESP32_Local_Net";
const char* AP_PASS     = "123456789";

IPAddress apIP(192, 168, 199, 1);
IPAddress netMask(255, 255, 255, 0);

WebServer server(80);
WiFiUDP dnsServer;
WiFiUDP dnsUpstream;
const uint16_t DNS_PORT = 53;
IPAddress publicDns(8, 8, 8, 8);

bool internetAllowed     = true;
bool naptActive          = false;
uint8_t lastStationCount = 255;

struct ClientActivity {
  IPAddress ip;
  String lastDomain;
  unsigned long lastActiveMs;
  uint32_t queryCount;
};

const int MAX_CLIENTS = 10;
ClientActivity trackedClients[MAX_CLIENTS];
int trackedCount = 0;

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
  int foundIndex = -1;
  for (int i = 0; i < trackedCount; i++) {
    if (trackedClients[i].ip == clientIP) {
      foundIndex = i;
      break;
    }
  }

  if (foundIndex != -1) {
    trackedClients[foundIndex].lastDomain = domain;
    trackedClients[foundIndex].lastActiveMs = millis();
    trackedClients[foundIndex].queryCount++;
  } else if (trackedCount < MAX_CLIENTS) {
    trackedClients[trackedCount].ip = clientIP;
    trackedClients[trackedCount].lastDomain = domain;
    trackedClients[trackedCount].lastActiveMs = millis();
    trackedClients[trackedCount].queryCount = 1;
    trackedCount++;
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

// REST API Handlers
void handleApiStatus() {
  String json = "{\"internet_allowed\":" + String(internetAllowed ? "true" : "false") + ",";
  json += "\"connected_clients\":" + String(WiFi.softAPgetStationNum()) + ",";
  json += "\"devices\":[";

  for (int i = 0; i < trackedCount; i++) {
    if (i > 0) json += ",";
    unsigned long secAgo = (millis() - trackedClients[i].lastActiveMs) / 1000;
    json += "{\"ip\":\"" + trackedClients[i].ip.toString() + "\",";
    json += "\"last_domain\":\"" + trackedClients[i].lastDomain + "\",";
    json += "\"queries\":" + String(trackedClients[i].queryCount) + ",";
    json += "\"last_active_sec\":" + String(secAgo) + "}";
  }
  json += "]}";

  server.send(200, "application/json", json);
}

void handleApiToggle() {
  internetAllowed = !internetAllowed;
  Serial.printf("[CONTROL] Internet toggled via API to: %s\n", internetAllowed ? "ENABLED" : "DISABLED");
  handleApiStatus();
}

void setup() {
  Serial.begin(74880);
  delay(1000);
  Serial.println("\n--- ESP32 NAT Router Booting ---");

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  WiFi.softAPConfig(apIP, apIP, netMask);
  WiFi.softAP(AP_SSID, AP_PASS);

  dnsServer.begin(DNS_PORT);

  // Configure CORS so local devices can query the API directly
  server.enableCORS(true);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/toggle", HTTP_POST, handleApiToggle);
  server.on("/api/toggle", HTTP_GET, handleApiToggle);
  server.begin();

  Serial.print("[AP] Gateway IP: ");
  Serial.println(WiFi.softAPIP());

  WiFi.begin(ROUTER_SSID, ROUTER_PASS);
  Serial.print("[STA] Connecting to ");
  Serial.println(ROUTER_SSID);
}

void loop() {
  server.handleClient();

  bool uplinkConnected = (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != IPAddress(0, 0, 0, 0));
  bool targetNaptState = uplinkConnected && internetAllowed;

  if (targetNaptState != naptActive) {
    naptActive = targetNaptState;
    WiFi.AP.enableNAPT(naptActive);
    if (naptActive) {
      Serial.println(">>> NAT ROUTER ACTIVE <<<");
    } else {
      Serial.println(">>> NAT ROUTER PAUSED <<<");
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

  delay(5);
}