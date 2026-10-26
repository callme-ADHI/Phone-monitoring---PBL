#include <WiFi.h>
#include <WiFiUdp.h>

const char* ROUTER_SSID = "Nobody_EXT";
const char* ROUTER_PASS = "DeathLord"; // Replace with your actual password

const char* AP_SSID     = "ESP32_Local_Net";
const char* AP_PASS     = "123456789";

IPAddress apIP(192, 168, 199, 1);
IPAddress netMask(255, 255, 255, 0);

WiFiUDP dnsServer;
WiFiUDP dnsUpstream;
const uint16_t DNS_PORT = 53;
IPAddress publicDns(8, 8, 8, 8);

bool naptEnabled = false;
uint8_t lastStationCount = 255;

// Non-blocking UDP DNS Proxy: forwards client DNS requests to 8.8.8.8
void processDnsProxy() {
  int packetSize = dnsServer.parsePacket();
  if (packetSize > 0) {
    uint8_t buffer[512];
    int bytesRead = dnsServer.read(buffer, sizeof(buffer));
    IPAddress clientIP = dnsServer.remoteIP();
    uint16_t clientPort = dnsServer.remotePort();

    if (bytesRead > 0) {
      dnsUpstream.beginPacket(publicDns, DNS_PORT);
      dnsUpstream.write(buffer, bytesRead);
      dnsUpstream.endPacket();

      // Non-blocking wait for upstream DNS reply (max 150ms)
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

void setup() {
  Serial.begin(74880); // Matches monitor_speed in platformio.ini
  delay(1000);
  Serial.println("\n--- ESP32 NAT Router Booting ---");

  // 1. Dual Mode & Power Management
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);               // Prevents STA disconnect flapping
  WiFi.setTxPower(WIFI_POWER_8_5dBm); // Limits power supply brownout spikes

  // 2. Configure Subnet & Start Access Point
  WiFi.softAPConfig(apIP, apIP, netMask);
  WiFi.softAP(AP_SSID, AP_PASS);

  // 3. Start DNS Proxy on Port 53
  dnsServer.begin(DNS_PORT);

  Serial.print("[AP] SoftAP Gateway IP: ");
  Serial.println(WiFi.softAPIP());

  // 4. Connect to Uplink Router
  WiFi.begin(ROUTER_SSID, ROUTER_PASS);
  Serial.print("[STA] Connecting to ");
  Serial.println(ROUTER_SSID);
}

void loop() {
  // 1. Thread-safe NAPT activation once connected with a valid IP
  bool isConnected = (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != IPAddress(0, 0, 0, 0));

  if (isConnected) {
    if (!naptEnabled) {
      Serial.println("\n[UPLINK] Connected & IP Assigned!");
      Serial.print("[STA] ESP32 Local IP: ");
      Serial.println(WiFi.localIP());

      if (WiFi.AP.enableNAPT(true)) {
        Serial.println(">>> FULL NAT ROUTER ACTIVE: Devices can browse the web <<<");
      } else {
        Serial.println("[ERROR] Failed to enable NAPT routing.");
      }
      naptEnabled = true;
    }

    // Process incoming DNS requests when uplink is active
    processDnsProxy();

  } else {
    if (naptEnabled) {
      Serial.println("\n[UPLINK] Internet Disconnected!");
      WiFi.AP.enableNAPT(false);
      naptEnabled = false;
    }
  }

  // 2. Log station count ONLY when a device connects or disconnects
  uint8_t currentStationCount = WiFi.softAPgetStationNum();
  if (currentStationCount != lastStationCount) {
    lastStationCount = currentStationCount;
    Serial.printf("[STATIONS] Active connected devices: %d\n", currentStationCount);
  }

  delay(5); // Small delay to yield execution time to background Wi-Fi tasks
}