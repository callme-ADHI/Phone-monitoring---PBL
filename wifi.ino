/*
 * ============================================================================
 * Project: ESP32 WiFi Router & Classroom Occupancy Monitor
 * File:    wifi.ino
 *
 * Target:
 *   ESP32 Dev Module (standard WROOM-32)
 *   Targeted Core: arduino-esp32 v3.x (v3.0.0 through v3.3.x, ESP-IDF 5.x based)
 *
 * Core Version & NAPT Notes:
 *   - In arduino-esp32 v3.x, lwIP NAPT functions (ip_napt_enable) are compiled
 *     directly into the prebuilt liblwip.a library.
 *   - In older arduino-esp32 v2.0.x cores, NAPT was omitted from standard
 *     precompiled libraries; if compiled on a core lacking NAPT symbols, the
 *     weak symbol linkage prevents linker crashes and allows graceful degradation
 *     (device counting, traffic monitoring, and web interface remain 100% operational).
 * ============================================================================
 */

#include <WiFi.h>
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
static const int   AP_MAX_CLIENTS = 10;
static const IPAddress apIP(192, 168, 4, 1);
static const IPAddress apNetmask(255, 255, 255, 0);

// Upstream WiFi settings (Internet connection)
static const char* STA_SSID = "VoiDX";
static const char* STA_PASS = "sunisgood";

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

// System state
static bool     napt_active = false;
static bool     sta_connected = false;
static uint32_t sta_last_retry_ms = 0;

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
  // Check if device already tracked
  for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
    if (tracked_devices[i].in_use && memcmp(tracked_devices[i].mac, mac, 6) == 0) {
      return &tracked_devices[i];
    }
  }

  // Allocate new slot or overwrite oldest
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

  if (len < 24) return; // Minimum 802.11 MAC header length

  uint8_t fc0 = payload[0];
  uint8_t fc1 = payload[1];

  // Verify Frame Type is Data (0b10 in bits 2..3)
  if ((fc0 & 0x0C) != 0x08) return;

  bool to_ds   = (fc1 & 0x01) != 0;
  bool from_ds = (fc1 & 0x02) != 0;

  const uint8_t* addr1 = payload + 4;   // Receiver Address
  const uint8_t* addr2 = payload + 10;  // Transmitter Address

  uint32_t now = millis();

  // Uplink: Phone -> ESP32 SoftAP (ToDS = 1, FromDS = 0)
  // Addr1 is BSSID (our AP MAC), Addr2 is Phone MAC
  if (to_ds && !from_ds) {
    if (memcmp(addr1, ap_mac, 6) == 0) {
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
  }
  // Downlink: ESP32 SoftAP -> Phone (ToDS = 0, FromDS = 1)
  // Addr1 is Phone MAC, Addr2 is BSSID (our AP MAC)
  else if (!to_ds && from_ds) {
    if (memcmp(addr2, ap_mac, 6) == 0) {
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
}

// ----------------------------------------------------------------------------
// Rate & Usage Calculation (Called once per second from loop)
// ----------------------------------------------------------------------------
static void update_rolling_rates() {
  uint32_t now = millis();
  portENTER_CRITICAL(&tracker_mux);

  // Advance circular bucket index
  uint8_t next_bucket = (curr_bucket_idx + 1) % ROLLING_WINDOW_SECS;

  for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
    if (!tracked_devices[i].in_use) continue;
    DeviceStats* dev = &tracked_devices[i];

    // Tally sum over all buckets in window
    uint32_t sum = 0;
    for (int b = 0; b < ROLLING_WINDOW_SECS; b++) {
      sum += dev->sec_bytes[b];
    }
    dev->recent_bytes_sum = sum;
    dev->recent_rate_bps = sum / ROLLING_WINDOW_SECS;

    // Reset the upcoming bucket for new second's samples
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
    // Enable NAPT on SoftAP interface address
    ip_napt_enable((uint32_t)apIP, 1);
    Serial.println(F("[ROUTER] NAPT enabled successfully on SoftAP interface!"));

    // Configure DNS server offered by SoftAP DHCP server
    IPAddress upstream_dns = WiFi.dnsIP();
    if (upstream_dns == IPAddress(0, 0, 0, 0)) {
      upstream_dns = IPAddress(8, 8, 8, 8); // Google public DNS fallback
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
    Serial.println(F("[ROUTER] Internet routing: UNAVAILABLE (operating in standalone AP mode)."));
    napt_active = false;
    return false;
  }
#else
  Serial.println(F("[ROUTER] NAPT disabled by ENABLE_NAPT setting."));
  napt_active = false;
  return false;
#endif
}

// ----------------------------------------------------------------------------
// Embedded Web Dashboard (HTML / CSS / JavaScript)
// 100% self-contained in PROGMEM; zero external CDN dependencies.
// ----------------------------------------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 WiFi Router & Monitor</title>
  <style>
    :root {
      --bg: #0b0f19;
      --card-bg: rgba(22, 31, 48, 0.7);
      --border: rgba(255, 255, 255, 0.08);
      --text: #f3f4f6;
      --text-muted: #9ca3af;
      --accent: #38bdf8;
      --green: #10b981;
      --amber: #f59e0b;
      --orange: #f97316;
      --red: #ef4444;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      background: var(--bg);
      color: var(--text);
      min-height: 100vh;
      padding: 1.5rem;
    }
    .container { max-width: 960px; margin: 0 auto; }
    header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 2rem;
      flex-wrap: wrap;
      gap: 1rem;
    }
    h1 { font-size: 1.5rem; font-weight: 700; letter-spacing: -0.02em; }
    .subtitle { color: var(--text-muted); font-size: 0.85rem; margin-top: 0.25rem; }
    .live-badge {
      display: inline-flex;
      align-items: center;
      gap: 0.5rem;
      background: rgba(16, 185, 129, 0.1);
      border: 1px solid rgba(16, 185, 129, 0.3);
      color: var(--green);
      font-size: 0.75rem;
      font-weight: 600;
      padding: 0.35rem 0.75rem;
      border-radius: 9999px;
    }
    .pulse-dot {
      width: 8px;
      height: 8px;
      background: var(--green);
      border-radius: 50%;
      box-shadow: 0 0 0 0 rgba(16, 185, 129, 0.7);
      animation: pulse 1.8s infinite;
    }
    @keyframes pulse {
      0% { box-shadow: 0 0 0 0 rgba(16, 185, 129, 0.7); }
      70% { box-shadow: 0 0 0 8px rgba(16, 185, 129, 0); }
      100% { box-shadow: 0 0 0 0 rgba(16, 185, 129, 0); }
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(210px, 1fr));
      gap: 1rem;
      margin-bottom: 2rem;
    }
    .card {
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 1.25rem;
      backdrop-filter: blur(12px);
    }
    .card-label {
      font-size: 0.75rem;
      color: var(--text-muted);
      text-transform: uppercase;
      letter-spacing: 0.05em;
      margin-bottom: 0.5rem;
    }
    .card-value { font-size: 1.75rem; font-weight: 700; }
    .card-subtext { font-size: 0.75rem; color: var(--text-muted); margin-top: 0.35rem; }
    .status-pill {
      display: inline-block;
      padding: 0.2rem 0.55rem;
      border-radius: 6px;
      font-size: 0.75rem;
      font-weight: 600;
    }
    .pill-active { background: rgba(16, 185, 129, 0.15); color: var(--green); }
    .pill-unavailable { background: rgba(239, 68, 68, 0.15); color: var(--red); }
    .pill-connecting { background: rgba(245, 158, 11, 0.15); color: var(--amber); }

    .table-card {
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 12px;
      overflow: hidden;
      margin-bottom: 1.5rem;
    }
    .table-header {
      padding: 1rem 1.25rem;
      border-bottom: 1px solid var(--border);
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .table-title { font-weight: 600; font-size: 1rem; }
    table { width: 100%; border-collapse: collapse; text-align: left; font-size: 0.85rem; }
    th {
      background: rgba(0, 0, 0, 0.2);
      color: var(--text-muted);
      font-weight: 600;
      padding: 0.75rem 1.25rem;
      font-size: 0.75rem;
      text-transform: uppercase;
      letter-spacing: 0.05em;
    }
    td { padding: 0.85rem 1.25rem; border-bottom: 1px solid var(--border); }
    tr:last-child td { border-bottom: none; }
    .anon-id {
      font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
      font-weight: 700;
      color: var(--accent);
      background: rgba(56, 189, 248, 0.1);
      padding: 0.2rem 0.45rem;
      border-radius: 4px;
      letter-spacing: 0.05em;
    }
    .badge-idle { background: rgba(156, 163, 175, 0.15); color: var(--text-muted); }
    .badge-light { background: rgba(56, 189, 248, 0.15); color: var(--accent); }
    .badge-active { background: rgba(249, 115, 22, 0.15); color: var(--orange); }
    .empty-state {
      padding: 2.5rem;
      text-align: center;
      color: var(--text-muted);
      font-size: 0.9rem;
    }
    footer {
      text-align: center;
      font-size: 0.75rem;
      color: var(--text-muted);
      margin-top: 2rem;
    }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div>
        <h1>ESP32 WiFi Router & Occupancy</h1>
        <div class="subtitle" id="net-info">AP: PBL-MC (192.168.4.1) &bull; Upstream: VoiDX</div>
      </div>
      <div class="live-badge">
        <div class="pulse-dot"></div>
        <span id="update-indicator">Live Polling</span>
      </div>
    </header>

    <div class="grid">
      <div class="card">
        <div class="card-label">Connected Devices</div>
        <div class="card-value" id="dev-count">0</div>
        <div class="card-subtext">Stations on PBL-MC</div>
      </div>
      <div class="card">
        <div class="card-label">Internet Routing (NAT)</div>
        <div class="card-value" id="routing-badge">
          <span class="status-pill pill-connecting">Checking...</span>
        </div>
        <div class="card-subtext" id="routing-sub">lwIP NAPT Status</div>
      </div>
      <div class="card">
        <div class="card-label">Upstream Connection</div>
        <div class="card-value" id="sta-badge">
          <span class="status-pill pill-connecting">Connecting...</span>
        </div>
        <div class="card-subtext" id="sta-ip-text">SSID: VoiDX</div>
      </div>
      <div class="card">
        <div class="card-label">Channel & Uptime</div>
        <div class="card-value" id="ch-uptime">Ch --</div>
        <div class="card-subtext" id="uptime-text">Uptime: 0s</div>
      </div>
    </div>

    <div class="table-card">
      <div class="table-header">
        <div class="table-title">Currently Connected Devices</div>
        <div class="card-subtext">Hashed Anonymous IDs &bull; No payload stored</div>
      </div>
      <div id="table-container">
        <table>
          <thead>
            <tr>
              <th>Device ID</th>
              <th>Uplink (Tx)</th>
              <th>Downlink (Rx)</th>
              <th>Recent Rate</th>
              <th>Activity State</th>
              <th>Last Active</th>
            </tr>
          </thead>
          <tbody id="device-rows">
            <tr><td colspan="6" class="empty-state">No devices currently connected to PBL-MC</td></tr>
          </tbody>
        </table>
      </div>
    </div>

    <footer>
      ESP32 Local Router &bull; Hardware: ESP32-WROOM-32 &bull; Privacy-Preserving Monitoring
    </footer>
  </div>

  <script>
    function fmtBytes(bytes) {
      if (bytes < 1024) return bytes + ' B';
      if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
      return (bytes / 1048576).toFixed(2) + ' MB';
    }

    function fmtUptime(sec) {
      const h = Math.floor(sec / 3600);
      const m = Math.floor((sec % 3600) / 60);
      const s = sec % 60;
      if (h > 0) return h + 'h ' + m + 'm ' + s + 's';
      if (m > 0) return m + 'm ' + s + 's';
      return s + 's';
    }

    async function pollStatus() {
      try {
        const res = await fetch('/api/status');
        if (!res.ok) throw new Error('HTTP ' + res.status);
        const data = await res.json();

        // Update counts & status
        document.getElementById('dev-count').innerText = data.connected_count;
        
        // Routing status
        const rb = document.getElementById('routing-badge');
        if (data.routing_status === 'active') {
          rb.innerHTML = '<span class="status-pill pill-active">Active</span>';
          document.getElementById('routing-sub').innerText = 'Forwarding traffic to internet';
        } else {
          rb.innerHTML = '<span class="status-pill pill-unavailable">Unavailable</span>';
          document.getElementById('routing-sub').innerText = 'Local AP only (no NAT)';
        }

        // Upstream STA status
        const sb = document.getElementById('sta-badge');
        const stext = document.getElementById('sta-ip-text');
        if (data.sta_status === 'connected') {
          sb.innerHTML = '<span class="status-pill pill-active">Connected</span>';
          stext.innerText = 'IP: ' + data.sta_ip;
        } else {
          sb.innerHTML = '<span class="status-pill pill-connecting">Connecting...</span>';
          stext.innerText = 'Retrying ' + data.sta_ssid;
        }

        // Channel and Uptime
        document.getElementById('ch-uptime').innerText = 'Ch ' + data.channel;
        document.getElementById('uptime-text').innerText = 'Uptime: ' + fmtUptime(data.uptime_secs);

        // Header IP details
        document.getElementById('net-info').innerText =
          'AP: ' + data.ap_ssid + ' (' + data.ap_ip + ') • Upstream: ' + data.sta_ssid +
          (data.sta_status === 'connected' ? ' (' + data.sta_ip + ')' : ' (disconnected)');

        // Render device rows
        const tbody = document.getElementById('device-rows');
        if (!data.devices || data.devices.length === 0) {
          tbody.innerHTML = '<tr><td colspan="6" class="empty-state">No devices currently connected to PBL-MC</td></tr>';
        } else {
          let rows = '';
          data.devices.forEach(d => {
            let badgeClass = 'badge-idle';
            if (d.usage === 'light') badgeClass = 'badge-light';
            if (d.usage === 'active') badgeClass = 'badge-active';

            rows += '<tr>' +
              '<td><span class="anon-id">DEV-' + d.id + '</span></td>' +
              '<td>' + d.up_pkts.toLocaleString() + ' <span style="color:var(--text-muted);font-size:0.75rem;">(' + fmtBytes(d.up_bytes) + ')</span></td>' +
              '<td>' + d.down_pkts.toLocaleString() + ' <span style="color:var(--text-muted);font-size:0.75rem;">(' + fmtBytes(d.down_bytes) + ')</span></td>' +
              '<td>' + (d.rate_bps / 1024).toFixed(1) + ' KB/s</td>' +
              '<td><span class="status-pill ' + badgeClass + '">' + d.usage.toUpperCase() + '</span></td>' +
              '<td>' + d.idle_secs + 's ago</td>' +
            '</tr>';
          });
          tbody.innerHTML = rows;
        }

        document.getElementById('update-indicator').innerText = 'Live Polling';
        document.getElementById('update-indicator').style.color = 'var(--green)';
      } catch (err) {
        document.getElementById('update-indicator').innerText = 'Reconnecting...';
        document.getElementById('update-indicator').style.color = 'var(--amber)';
      }
    }

    // Poll every 1.5 seconds
    setInterval(pollStatus, 1500);
    pollStatus();
  </script>
</body>
</html>
)rawliteral";

// ----------------------------------------------------------------------------
// Web Server Request Handlers
// ----------------------------------------------------------------------------
static void handle_root() {
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handle_status_api() {
  // Query authoritative station list from ESP32 WiFi driver
  wifi_sta_list_t sta_list;
  esp_err_t err = esp_wifi_ap_get_sta_list(&sta_list);
  uint8_t connected_count = (err == ESP_OK) ? sta_list.num : 0;

  uint32_t now = millis();

  // Build JSON response
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

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// ----------------------------------------------------------------------------
// Setup
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1000);
  Serial.println(F("\n=================================================="));
  Serial.println(F("    ESP32 WiFi Router & Occupancy Monitor         "));
  Serial.println(F("=================================================="));

  // Generate per-boot hardware random salt for MAC anonymization
  boot_salt = esp_random();
  Serial.printf("[PRIVACY] Hardware salt generated (0x%08X)\n", boot_salt);

  // 1. Set dual AP + Station mode
  WiFi.mode(WIFI_AP_STA);

  // 2. Configure AP static IP
  WiFi.softAPConfig(apIP, apIP, apNetmask);

  // 3. AP Password Length Handling (WPA2 requires min 8 characters)
  size_t pass_len = strlen(AP_PASS);
  if (pass_len > 0 && pass_len < 8) {
    Serial.println(F("\n[WARNING] ----------------------------------------------------"));
    Serial.printf("[WARNING] Configured AP password '%s' is only %d characters!\n", AP_PASS, (int)pass_len);
    Serial.println(F("[WARNING] WPA2 specification requires a minimum of 8 characters."));
    Serial.println(F("[WARNING] Starting 'PBL-MC' as an OPEN network (no password)."));
    Serial.println(F("[WARNING] Change AP_PASS to 8+ characters to secure this network."));
    Serial.println(F("[WARNING] ----------------------------------------------------\n"));

    WiFi.softAP(AP_SSID, nullptr, 1, 0, AP_MAX_CLIENTS);
  } else if (pass_len >= 8) {
    WiFi.softAP(AP_SSID, AP_PASS, 1, 0, AP_MAX_CLIENTS);
  } else {
    WiFi.softAP(AP_SSID, nullptr, 1, 0, AP_MAX_CLIENTS);
  }

  WiFi.softAPmacAddress(ap_mac);
  Serial.printf("[AP] Access Point started: SSID='%s' IP=%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  // 4. Start Promiscuous Sniffer for Traffic Header Analysis
  wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA };
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_rx_callback);
  esp_wifi_set_promiscuous(true);
  Serial.println(F("[SNIFFER] Promiscuous 802.11 data frame header monitor active."));

  // 5. Connect to upstream WiFi (Non-blocking: initiates background connection)
  Serial.printf("[STA] Connecting to upstream WiFi '%s'...\n", STA_SSID);
  WiFi.begin(STA_SSID, STA_PASS);

  // 6. Start Web Server
  server.on("/", HTTP_GET, handle_root);
  server.on("/api/status", HTTP_GET, handle_status_api);
  server.begin();
  Serial.println(F("[HTTP] Web dashboard server started on port 80."));
  Serial.printf("[HTTP] Access from phone on PBL-MC: http://%s/\n", WiFi.softAPIP().toString().c_str());
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

  // 2. Check STA connection status & manage NAPT routing state
  if (WiFi.status() == WL_CONNECTED) {
    if (!sta_connected) {
      sta_connected = true;
      Serial.println(F("\n[STA] Upstream WiFi connected!"));
      Serial.printf("[STA] ESP32 IP on '%s': %s\n", STA_SSID, WiFi.localIP().toString().c_str());
      Serial.printf("[STA] Channel in use: %d\n", WiFi.channel());

      // Attempt to enable NAPT routing
      init_napt_routing();
    }
  } else {
    if (sta_connected) {
      sta_connected = false;
      napt_active = false;
      Serial.println(F("\n[STA] Upstream WiFi disconnected. Routing unavailable."));
    }

    // Auto-retry connection every 10 seconds without blocking the loop
    if (now - sta_last_retry_ms >= 10000UL) {
      sta_last_retry_ms = now;
      Serial.printf("[STA] Retrying connection to '%s'...\n", STA_SSID);
      WiFi.begin(STA_SSID, STA_PASS);
    }
  }
}
