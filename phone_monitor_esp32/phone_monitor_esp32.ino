/*
  Classroom Phone Monitor - ESP32, WiFi only (no BLE)

  Counts devices by listening for WiFi probe requests and shows the result on a
  web dashboard served by the ESP32's own access point.

  Connect to the WiFi "detect", then open http://192.168.4.1
  (with the captive portal on, the phone usually opens the page by itself).

  Board:  any ESP32 (Arduino-ESP32 core 2.x or 3.x)
  Tools:  default partition scheme is fine now that BLE is gone.
*/

// ---------------- settings ----------------
#define WEB_AUTH        0   // 1 = ask for user/password (can break phone captive-portal pop-ups)
#define CAPTIVE_PORTAL  1   // 1 = phones auto-open the dashboard after joining "detect"
#define SERIAL_LOG      1   // 1 = print a status line every 10 s

static const char* AP_SSID = "detect";
static const char* AP_PASS = "phonemon123";   // 8+ chars = WPA2. Use "" for an open network.
static const int   AP_CHANNEL = 6;            // probe requests are counted on this channel
static const int   AP_MAX_CLIENTS = 4;

static const char* WEB_USER = "admin";        // only used if WEB_AUTH is 1
static const char* WEB_PASSWORD = "admin123";

static const int DEFAULT_RSSI = -75;
static const int DEFAULT_WINDOW_S = 60;
// ------------------------------------------

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_wifi.h>

#define TBL_SIZE 512
#define HIST_LEN 180
#define NET_MAX  40

struct Entry  { uint32_t h; uint32_t t; int8_t rssi; uint8_t rnd; };
struct Counts { int total; int rnd; int hist[7]; };
struct Sample { uint16_t total, rnd; };
struct Net    { char ssid[33]; int8_t rssi; uint8_t ch; uint8_t enc; };

static Entry wifiTbl[TBL_SIZE];
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t salt;
static volatile int rssiMin = DEFAULT_RSSI;
static volatile uint32_t windowMs = DEFAULT_WINDOW_S * 1000UL;
static volatile uint32_t probeTotal = 0;

static Sample history[HIST_LEN];
static int histHead = 0, histCount = 0;
static int probeRate = 0;

static Net nets[NET_MAX];
static int netCount = 0;
static bool scanReq = false;
static bool scanBusy = false;
static uint32_t scanStartedAt = 0;
static uint32_t scanDoneAt = 0;

static IPAddress apIP(192, 168, 4, 1);

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

// ---------------- device table ----------------
static uint32_t hashMac(const uint8_t* m) {
  uint32_t h = 2166136261u ^ salt;
  for (int i = 0; i < 6; i++) { h ^= m[i]; h *= 16777619u; }
  return h ? h : 1;
}

static void touch(Entry* tbl, uint32_t h, int8_t rssi, uint8_t rnd) {
  uint32_t now = millis();
  if (!now) now = 1;
  int slot = 0;
  uint32_t maxAge = 0;
  portENTER_CRITICAL(&mux);
  for (int i = 0; i < TBL_SIZE; i++) {
    if (tbl[i].t && tbl[i].h == h) {
      tbl[i].t = now;
      tbl[i].rssi = rssi;
      portEXIT_CRITICAL(&mux);
      return;
    }
    uint32_t age = tbl[i].t ? now - tbl[i].t : UINT32_MAX;
    if (age >= maxAge) { maxAge = age; slot = i; }
  }
  tbl[slot].h = h;
  tbl[slot].t = now;
  tbl[slot].rssi = rssi;
  tbl[slot].rnd = rnd;
  portEXIT_CRITICAL(&mux);
}

static Counts countTbl(Entry* tbl) {
  Counts c;
  memset(&c, 0, sizeof(c));
  uint32_t now = millis();
  uint32_t w = windowMs;
  portENTER_CRITICAL(&mux);
  for (int i = 0; i < TBL_SIZE; i++) {
    if (tbl[i].t && now - tbl[i].t < w) {
      c.total++;
      if (tbl[i].rnd) c.rnd++;
      int b = (tbl[i].rssi + 100) / 10;
      if (b < 0) b = 0;
      if (b > 6) b = 6;
      c.hist[b]++;
    }
  }
  portEXIT_CRITICAL(&mux);
  return c;
}

static void clearTables() {
  portENTER_CRITICAL(&mux);
  memset(wifiTbl, 0, sizeof(wifiTbl));
  portEXIT_CRITICAL(&mux);
}

// ---------------- sniffer ----------------
static void sniffCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  if (p->rx_ctrl.sig_len < 24) return;          // too short to be a real frame
  const uint8_t* f = p->payload;
  if (f[0] != 0x40) return;                     // 0x40 = probe request
  probeTotal = probeTotal + 1;
  int rssi = p->rx_ctrl.rssi;
  if (rssi < rssiMin) return;
  const uint8_t* sa = f + 10;                   // source address
  touch(wifiTbl, hashMac(sa), (int8_t)rssi, (sa[0] & 0x02) ? 1 : 0);
}

static void sniffOn() {
  wifi_promiscuous_filter_t flt;
  flt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&flt);
  esp_wifi_set_promiscuous_rx_cb(&sniffCb);
  esp_wifi_set_promiscuous(true);
}

// ---------------- WiFi ----------------
static void startWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);       // STA side stays idle; it is only needed for network scans
  WiFi.setSleep(false);         // keeps the access point responsive
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  bool secured = strlen(AP_PASS) >= 8;
  if (strlen(AP_PASS) > 0 && !secured)
    Serial.println("AP_PASS must be 8+ characters for WPA2 - starting an OPEN network instead");

  bool ok = false;
  for (int i = 0; i < 3 && !ok; i++) {
    ok = WiFi.softAP(AP_SSID, secured ? AP_PASS : nullptr, AP_CHANNEL, 0, AP_MAX_CLIENTS);
    if (!ok) delay(300);
  }
  if (!ok) Serial.println("softAP failed to start");
  delay(200);

#if CAPTIVE_PORTAL
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", apIP);
#endif
  if (MDNS.begin("phonemon")) MDNS.addService("http", "tcp", 80);
}

static const char* encName(uint8_t e) {
  switch (e) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Ent";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "Other";
  }
}

// Non-blocking network scan: the web page keeps working while it runs.
static void scanStart() {
  esp_wifi_set_promiscuous(false);
  WiFi.scanDelete();
  int r = WiFi.scanNetworks(true, true);        // async, show hidden
  if (r == WIFI_SCAN_FAILED) {
    sniffOn();
    return;
  }
  scanBusy = true;
  scanStartedAt = millis();
}

static void scanPoll() {
  if (!scanBusy) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    if (millis() - scanStartedAt > 20000) {     // safety timeout
      WiFi.scanDelete();
      scanBusy = false;
      sniffOn();
    }
    return;
  }
  if (n >= 0) {
    netCount = 0;
    for (int i = 0; i < n && netCount < NET_MAX; i++) {
      Net& x = nets[netCount++];
      strncpy(x.ssid, WiFi.SSID(i).c_str(), 32);
      x.ssid[32] = 0;
      x.rssi = (int8_t)WiFi.RSSI(i);
      x.ch = (uint8_t)WiFi.channel(i);
      x.enc = (uint8_t)WiFi.encryptionType(i);
    }
    scanDoneAt = millis();
    if (!scanDoneAt) scanDoneAt = 1;
  }
  WiFi.scanDelete();
  scanBusy = false;
  sniffOn();
}

// ---------------- web handlers ----------------
static void addEsc(String& out, const char* s) {
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 32) out += ' ';
    else out += c;
  }
}

static bool authed() {
#if WEB_AUTH
  if (!server.authenticate(WEB_USER, WEB_PASSWORD)) {
    server.requestAuthentication();
    return false;
  }
#endif
  return true;
}

extern const char PAGE[];

static void handleRoot() {
  if (!authed()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", PAGE);
}

static void handleStatus() {
  if (!authed()) return;
  Counts w = countTbl(wifiTbl);
  uint8_t ch = 0;
  wifi_second_chan_t sec;
  esp_wifi_get_channel(&ch, &sec);
  char buf[400];
  snprintf(buf, sizeof(buf),
    "{\"up\":%lu,\"ch\":%d,\"total\":%d,\"rnd\":%d,\"probes\":%lu,\"rate\":%d,"
    "\"rssi\":%d,\"win\":%lu,\"heap\":%u,\"clients\":%d,\"scanning\":%d,"
    "\"hist\":[%d,%d,%d,%d,%d,%d,%d]}",
    (unsigned long)(millis() / 1000), (int)ch, w.total, w.rnd,
    (unsigned long)probeTotal, probeRate, (int)rssiMin, (unsigned long)(windowMs / 1000),
    (unsigned)ESP.getFreeHeap(), WiFi.softAPgetStationNum(),
    (scanBusy || scanReq) ? 1 : 0,
    w.hist[0], w.hist[1], w.hist[2], w.hist[3], w.hist[4], w.hist[5], w.hist[6]);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

static void handleHistory() {
  if (!authed()) return;
  String out;
  out.reserve(histCount * 12 + 4);
  out += '[';
  int start = (histHead - histCount + HIST_LEN) % HIST_LEN;
  for (int i = 0; i < histCount; i++) {
    const Sample& s = history[(start + i) % HIST_LEN];
    if (i) out += ',';
    out += '[';
    out += String(s.total); out += ',';
    out += String(s.rnd);
    out += ']';
  }
  out += ']';
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

static void handleScan() {
  if (!authed()) return;
  if (!scanBusy) scanReq = true;
  server.send(200, "application/json", "{\"scanning\":true}");
}

static void handleNetworks() {
  if (!authed()) return;
  String out;
  out.reserve(64 + netCount * 70);
  out += "{\"scanning\":";
  out += (scanBusy || scanReq) ? "true" : "false";
  out += ",\"age\":";
  out += scanDoneAt ? String((millis() - scanDoneAt) / 1000) : String(-1);
  out += ",\"nets\":[";
  for (int i = 0; i < netCount; i++) {
    if (i) out += ',';
    out += "{\"s\":\"";
    addEsc(out, nets[i].ssid);
    out += "\",\"c\":";
    out += String(nets[i].ch);
    out += ",\"r\":";
    out += String((int)nets[i].rssi);
    out += ",\"e\":\"";
    out += encName(nets[i].enc);
    out += "\"}";
  }
  out += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

static void handleConfig() {
  if (!authed()) return;
  if (server.hasArg("rssi")) {
    int v = constrain((int)server.arg("rssi").toInt(), -100, -30);
    rssiMin = v;
    prefs.putInt("rssi", v);
  }
  if (server.hasArg("win")) {
    int s = constrain((int)server.arg("win").toInt(), 10, 300);
    windowMs = s * 1000UL;
    prefs.putInt("win", s);
  }
  handleStatus();
}

static void handleClear() {
  if (!authed()) return;
  clearTables();
  histHead = 0;
  histCount = 0;
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleNotFound() {
#if CAPTIVE_PORTAL
  // Phones probe URLs like /generate_204; redirecting makes them show the dashboard.
  String url = String("http://") + apIP.toString() + "/";
  server.sendHeader("Location", url, true);
  server.send(302, "text/plain", "");
#else
  server.send(404, "text/plain", "not found");
#endif
}

// ---------------- periodic work ----------------
static void sampleTick() {
  static uint32_t tLast = 0;
  static uint32_t lastProbe = 0;
  uint32_t now = millis();
  if (now - tLast < 5000) return;
  tLast = now;
  Counts w = countTbl(wifiTbl);
  Sample& s = history[histHead];
  s.total = w.total;
  s.rnd = w.rnd;
  histHead = (histHead + 1) % HIST_LEN;
  if (histCount < HIST_LEN) histCount++;
  uint32_t p = probeTotal;
  probeRate = (int)((p - lastProbe) * 12);      // per 5 s -> per minute
  lastProbe = p;
}

static void logTick() {
#if SERIAL_LOG
  static uint32_t tLast = 0;
  uint32_t now = millis();
  if (now - tLast < 10000) return;
  tLast = now;
  Counts w = countTbl(wifiTbl);
  Serial.printf("devices=%d random=%d probes=%lu viewers=%d heap=%u\n",
                w.total, w.rnd, (unsigned long)probeTotal,
                WiFi.softAPgetStationNum(), (unsigned)ESP.getFreeHeap());
#endif
}

void setup() {
  Serial.begin(115200);
  delay(300);
  salt = esp_random();
  prefs.begin("pm", false);
  rssiMin = constrain(prefs.getInt("rssi", DEFAULT_RSSI), -100, -30);
  windowMs = (uint32_t)constrain(prefs.getInt("win", DEFAULT_WINDOW_S), 10, 300) * 1000UL;

  startWiFi();
  sniffOn();

  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/history", handleHistory);
  server.on("/api/scan", handleScan);
  server.on("/api/networks", handleNetworks);
  server.on("/api/config", handleConfig);
  server.on("/api/clear", handleClear);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println();
  Serial.printf("Network: %s  password: %s\n", AP_SSID, strlen(AP_PASS) >= 8 ? AP_PASS : "(open)");
  Serial.printf("Dashboard: http://%s\n", WiFi.softAPIP().toString().c_str());
}

void loop() {
  server.handleClient();
#if CAPTIVE_PORTAL
  dnsServer.processNextRequest();
#endif
  sampleTick();
  if (scanReq && !scanBusy) {
    scanReq = false;
    scanStart();
  }
  scanPoll();
  logTick();
  delay(2);
}

// ---------------- dashboard page ----------------
const char PAGE[] = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Phone Monitor</title>
<style>
:root{--bg:#f4f6f9;--card:#fff;--ink:#1c2430;--mute:#6b7785;--acc:#2878a0;--navy:#19375a;--warn:#c0392b}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,Segoe UI,sans-serif;background:var(--bg);color:var(--ink)}
header{background:var(--navy);color:#fff;padding:14px 18px;display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap}
h1{font-size:1.1rem;margin:0}
#st{font-size:.85rem;opacity:.85}
main{max-width:980px;margin:0 auto;padding:14px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:10px;margin-bottom:12px}
.card{background:var(--card);border-radius:10px;padding:14px;box-shadow:0 1px 3px #0002;margin-bottom:12px}
.grid .card{margin:0}
.k{color:var(--mute);font-size:.8rem}
.v{font-size:2.1rem;font-weight:700;line-height:1.2}
.s{color:var(--mute);font-size:.75rem}
h2{font-size:.95rem;margin:0 0 10px}
canvas{width:100%;height:220px;display:block}
.lg{font-size:.78rem;color:var(--mute);margin-top:6px}
.lg b{display:inline-block;width:10px;height:10px;border-radius:2px;margin:0 4px 0 10px;vertical-align:middle}
.two{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media(max-width:700px){.two{grid-template-columns:1fr}}
.row{display:flex;align-items:center;gap:8px;font-size:.78rem;margin:3px 0}
.row span:first-child{width:62px;color:var(--mute)}
.bar{height:12px;background:var(--acc);border-radius:3px;min-width:1px}
.row .n{width:28px;text-align:right}
button,select,input{font:inherit}
button{background:var(--acc);color:#fff;border:0;border-radius:6px;padding:7px 14px;cursor:pointer}
button.g{background:#8794a3}
button:disabled{opacity:.5}
.ctl{display:flex;flex-wrap:wrap;gap:12px;align-items:center}
table{width:100%;border-collapse:collapse;font-size:.85rem}
th,td{text-align:left;padding:5px 6px;border-bottom:1px solid #eef1f4}
.open{color:var(--warn);font-weight:600}
.note{font-size:.75rem;color:var(--mute);margin-top:8px}
#rec{font-weight:600;color:var(--navy)}
</style></head><body>
<header><h1>Classroom Phone Monitor</h1><span id="st">connecting...</span></header>
<main>
<div class="grid">
<div class="card"><div class="k">Devices (estimate)</div><div class="v" id="total">-</div><div class="s">unique WiFi devices in the window</div></div>
<div class="card"><div class="k">Random address</div><div class="v" id="rnd">-</div><div class="s">phones hiding their real MAC</div></div>
<div class="card"><div class="k">Fixed address</div><div class="v" id="fix">-</div><div class="s">real MAC visible</div></div>
<div class="card"><div class="k">Probe requests / min</div><div class="v" id="rate">-</div><div class="s" id="probes">&nbsp;</div></div>
</div>

<div class="card"><h2>Last 15 minutes</h2><canvas id="chart"></canvas>
<div class="lg"><b style="background:#19375a"></b>devices<b style="background:#e08a00"></b>random address<span id="stats" style="float:right"></span></div></div>

<div class="two">
<div class="card"><h2>Signal strength of devices (dBm)</h2><div id="bars"></div>
<div class="note">Stronger signal usually means closer to the ESP32.</div></div>
<div class="card"><h2>Settings</h2>
<div class="ctl"><label>Minimum signal <b id="rv">-75</b> dBm<br><input id="rssi" type="range" min="-100" max="-30" value="-75"></label>
<label>Count window<br><select id="win"><option value="30">30 s</option><option value="60">60 s</option><option value="120">2 min</option><option value="300">5 min</option></select></label></div>
<div class="ctl" style="margin-top:10px"><button id="apply">Apply</button><button class="g" id="clr">Clear counts</button></div>
<div class="note">Raise the minimum signal to ignore phones outside the room. Changes take up to one window to show fully.</div></div>
</div>

<div class="card"><h2>WiFi environment</h2>
<div class="ctl"><button id="scan">Scan nearby networks</button><span id="scanMsg" class="s"></span></div>
<div id="netbox" style="display:none">
<div id="chan" style="margin:12px 0"></div>
<div id="recline" class="s"></div>
<div style="overflow-x:auto;margin-top:8px"><table><thead><tr><th>Network</th><th>Ch</th><th>dBm</th><th>Security</th></tr></thead><tbody id="nets"></tbody></table></div>
</div>
<div class="note">A scan takes a few seconds. Phone counting and your connection may pause briefly while it runs.</div></div>

<div class="note" style="text-align:center;margin-bottom:20px">Counts only. No MAC addresses are stored or shown.</div>
</main>
<script>
const $=id=>document.getElementById(id);
const get=async u=>{const r=await fetch(u,{cache:'no-store'});if(!r.ok)throw 0;return r.json()};
const HL=180;let hist=[],first=true,apCh=0;
const LBL=['<-90','-90/-80','-80/-70','-70/-60','-60/-50','-50/-40','>-40'];
$('bars').innerHTML=LBL.map((l,i)=>'<div class="row"><span>'+l+'</span><div class="bar" id="b'+i+'" style="width:0"></div><span class="n" id="n'+i+'">0</span></div>').join('');
const fmt=s=>s<60?s+'s':s<3600?Math.floor(s/60)+'m':Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';

async function poll(){
 try{
  const s=await get('/api/status');
  apCh=s.ch;
  $('total').textContent=s.total;
  $('rnd').textContent=s.rnd;
  $('fix').textContent=s.total-s.rnd;
  $('rate').textContent=s.rate;
  $('probes').textContent=s.probes+' since boot';
  $('st').textContent='live | channel '+s.ch+' | up '+fmt(s.up)+' | '+s.clients+' viewer'+(s.clients==1?'':'s');
  const mx=Math.max(1,...s.hist);
  s.hist.forEach((v,i)=>{$('b'+i).style.width=(v/mx*100)+'%';$('n'+i).textContent=v});
  if(first){first=false;$('rssi').value=s.rssi;$('rv').textContent=s.rssi;$('win').value=s.win}
 }catch(e){$('st').textContent='offline - retrying'}
}

async function loadHist(){
 try{hist=await get('/api/history');draw()}catch(e){}
}

function draw(){
 const c=$('chart'),d=window.devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;
 c.width=w*d;c.height=h*d;
 const g=c.getContext('2d');g.scale(d,d);g.clearRect(0,0,w,h);
 const n=hist.length;if(n<2){g.fillStyle='#6b7785';g.font='13px sans-serif';g.fillText('collecting data...',12,24);$('stats').textContent='';return}
 let mx=5;hist.forEach(p=>{mx=Math.max(mx,p[0],p[1])});mx=Math.ceil(mx/5)*5;
 const L=30,R=w-8,T=8,B=h-18;
 g.strokeStyle='#e3e8ee';g.fillStyle='#6b7785';g.font='11px sans-serif';g.lineWidth=1;
 for(let i=0;i<=4;i++){const y=T+(B-T)*i/4;g.beginPath();g.moveTo(L,y);g.lineTo(R,y);g.stroke();g.fillText(Math.round(mx*(1-i/4)),2,y+4)}
 g.fillText('-'+Math.round(n*5/60)+' min',L,h-4);g.fillText('now',R-22,h-4);
 [[1,'#e08a00',1.5],[0,'#19375a',2.5]].forEach(([k,col,lw])=>{
  g.strokeStyle=col;g.lineWidth=lw;g.beginPath();
  hist.forEach((p,i)=>{const x=R-(n-1-i)*(R-L)/(HL-1),y=B-(B-T)*p[k]/mx;i?g.lineTo(x,y):g.moveTo(x,y)});
  g.stroke()});
 const ph=hist.map(p=>p[0]),avg=(ph.reduce((a,b)=>a+b,0)/ph.length).toFixed(1);
 $('stats').textContent='devices min '+Math.min(...ph)+' / avg '+avg+' / max '+Math.max(...ph);
}

$('rssi').oninput=()=>{$('rv').textContent=$('rssi').value};
$('apply').onclick=async()=>{try{await get('/api/config?rssi='+$('rssi').value+'&win='+$('win').value);poll()}catch(e){}};
$('clr').onclick=async()=>{try{await get('/api/clear');hist=[];draw();poll()}catch(e){}};

$('scan').onclick=async()=>{
 $('scan').disabled=true;$('scanMsg').textContent='scanning...';
 try{await get('/api/scan')}catch(e){}
 const t=setInterval(async()=>{
  try{const d=await get('/api/networks');
   if(!d.scanning){clearInterval(t);$('scan').disabled=false;$('scanMsg').textContent='done';renderNets(d.nets)}
  }catch(e){}
 },1500);
};

function score(list,c){let s=0;list.forEach(n=>{const dd=Math.abs(n.c-c);if(dd<5)s+=Math.max(0,100+n.r)*(1-dd/5)});return s}

function renderNets(list){
 $('netbox').style.display='block';
 const cnt={};for(let i=1;i<=13;i++)cnt[i]=0;
 list.forEach(n=>{if(cnt[n.c]!==undefined)cnt[n.c]++});
 const mx=Math.max(1,...Object.values(cnt));
 const box=$('chan');box.innerHTML='';
 for(let i=1;i<=13;i++){
  const r=document.createElement('div');r.className='row';
  const a=document.createElement('span');a.textContent='ch '+i+(i==apCh?' *':'');
  const b=document.createElement('div');b.className='bar';b.style.width=(cnt[i]/mx*100)+'%';
  const c=document.createElement('span');c.className='n';c.textContent=cnt[i];
  r.append(a,b,c);box.append(r)}
 const sc=[1,6,11].map(c=>[c,score(list,c)]).sort((a,b)=>a[1]-b[1]);
 $('recline').innerHTML='Least crowded of channels 1, 6, 11: <span id="rec">'+sc[0][0]+'</span> (* marks the channel this ESP32 is using)';
 const tb=$('nets');tb.innerHTML='';
 list.slice().sort((a,b)=>b.r-a.r).forEach(n=>{
  const tr=document.createElement('tr');
  [n.s||'(hidden)',n.c,n.r,n.e].forEach((v,i)=>{const td=document.createElement('td');td.textContent=v;if(i==3&&v=='Open')td.className='open';tr.append(td)});
  tb.append(tr)});
}

poll();loadHist();
setInterval(poll,2000);setInterval(loadHist,5000);
window.addEventListener('resize',draw);
</script></body></html>)rawliteral";
