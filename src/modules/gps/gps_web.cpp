#include "gps_web.h"

#include "gps_provider.h"
#include "gps_casic.h"
#include "gps_types.h"
#include <globals.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>

static AsyncWebServer *gpsServer = nullptr;
static bool running = false;
static uint16_t listenPort = 8081;
static IPAddress staIp;
static IPAddress apIp;

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>GPS Monitor</title>
<style>
:root { color-scheme: light dark; font-family: Inter, system-ui, sans-serif; }
body { margin:16px; background:#0c1116; color:#e8f0f8; }
section { margin-bottom:18px; padding:12px; border:1px solid #23303f; border-radius:10px; background:#111925; }
button, select { padding:6px 10px; margin:2px; border-radius:8px; border:1px solid #3b536b; background:#1b2635; color:#e8f0f8; }
input { padding:6px 8px; border-radius:8px; border:1px solid #3b536b; background:#0f161f; color:#e8f0f8; }
#sats table { width:100%; border-collapse:collapse; font-size:13px; }
#sats th, #sats td { padding:4px 6px; border-bottom:1px solid #223042; }
.badge { padding:2px 6px; border-radius:6px; margin-left:6px; font-size:12px; }
.badge.yes { background:#135f2d; }
.badge.no { background:#5f1321; }
.bar { height:16px; background:linear-gradient(90deg,#234b92,#24c2e5); border-radius:4px; }
#log { max-height:220px; overflow-y:auto; font-family:ui-monospace, SFMono-Regular, Menlo, monospace; background:#0a0f18; padding:10px; border-radius:8px; border:1px solid #23303f; white-space:pre-wrap; }
.flex { display:flex; gap:10px; flex-wrap:wrap; }
.card { flex:1 1 260px; }
</style>
</head>
<body>
<h2>GPS / CASIC Web Monitor</h2>
<section id="summary" class="flex"></section>
<section id="settings" class="flex"></section>
<section id="controls">
  <h3>Controls</h3>
  <div class="flex">
    <div class="card">
      <label>Source:</label>
      <select id="sourceSel">
        <option value="legacy">Legacy (TinyGPS++)</option>
        <option value="casic">CASIC UART</option>
      </select>
      <button onclick="sendCmd('source', sourceSel.value)">Apply</button><br/>
      <label>Baud:</label>
      <select id="baudSel">
        <option>9600</option><option>19200</option><option>38400</option><option>57600</option><option selected>115200</option>
      </select>
      <button onclick="sendCmd('baud', baudSel.value)">Apply</button><br/>
      <label>Rate (ms):</label>
      <select id="rateSel"><option>1000</option><option>500</option><option>250</option><option>200</option><option>100</option></select>
      <button onclick="sendCmd('rate', rateSel.value)">Apply</button>
    </div>
    <div class="card">
      <label>Systems:</label>
      <select id="sysSel"><option value="1">GPS</option><option value="2">BDS</option><option value="3">GPS+BDS</option><option value="4">GLONASS</option><option value="5">GPS+GLN</option><option value="6">BDS+GLN</option><option value="7">GPS+BDS+GLN</option></select>
      <button onclick="sendCmd('system', sysSel.value)">Apply</button><br/>
      <label>NMEA ver:</label>
      <select id="nmeaSel"><option value="9">2.2</option><option value="2">4.1</option></select>
      <button onclick="sendCmd('nmea', nmeaSel.value)">Apply</button>
    </div>
    <div class="card">
      <button onclick="sendCmd('reset','hot')">Hot reset</button>
      <button onclick="sendCmd('reset','warm')">Warm</button>
      <button onclick="sendCmd('reset','cold')">Cold</button>
      <button onclick="sendCmd('reset','factory')">Factory</button><br/>
      <button onclick="sendCmd('save','')">Save config</button>
      <button onclick="sendCmd('info','')">Query info</button>
      <button onclick="sendCmd('log','toggle')" id="logBtn">Log off</button>
    </div>
  </div>
</section>
<section id="sats">
  <h3>Satellites</h3>
  <div id="satSummary"></div>
  <div id="bars" style="display:grid;gap:6px;margin:8px 0;"></div>
  <table>
    <thead><tr><th>SYS</th><th>SVID</th><th>C/N0</th><th>Signal</th><th>El</th><th>Az</th><th>Used</th></tr></thead>
    <tbody id="satRows"></tbody>
  </table>
</section>
<section>
  <h3>NMEA / status log</h3>
  <div id="log"></div>
</section>
<script>
const logBox = document.getElementById('log');
function qual(c){return c>=40?'excellent':c>=30?'good':c>=25?'ok':c>=20?'weak':'very weak';}
function update(){fetch('/api/status').then(r=>r.json()).then(j=>{
  document.getElementById('logBtn').textContent = j.log_enabled? 'Log off':'Log on';
  const sum = document.getElementById('summary');
  const timeTxt = j.time.valid ? `${String(j.time.hh).padStart(2,'0')}:${String(j.time.mm).padStart(2,'0')}:${String(j.time.ss).padStart(2,'0')} UTC` : 'unknown';
  sum.innerHTML = `
    <div class="card"><h3>Fix</h3><div>${j.fix.valid?'YES':'NO'} (mode ${j.fix.mode})</div>
    <div>${j.fix.valid?`Lat ${j.fix.lat.toFixed(6)}<br>Lon ${j.fix.lon.toFixed(6)}<br>Alt ${j.fix.alt.toFixed(1)} m`:'No position'}</div>
    <div>Time: ${timeTxt}</div></div>
    <div class="card"><h3>Satellites</h3><div>Used ${j.fix.sats_used} / ${j.sats.length}</div>
    <div>GPS ${j.sys.GPS} BDS ${j.sys.BDS} GLN ${j.sys.GLN} GAL ${j.sys.GAL} QZ ${j.sys.QZ} SB ${j.sys.SB}</div>
    <div>Antenna: ${j.antenna}</div></div>
    <div class="card"><h3>Motion</h3><div>Speed ${j.fix.speed_kph.toFixed(1)} km/h</div><div>Course ${j.fix.course_deg.toFixed(1)}°</div></div>`;
  const set = document.getElementById('settings');
  set.innerHTML = `
    <div class="card"><h3>Source</h3><div>${j.config.source}</div>
      <div>Pins RX ${j.config.rx} / TX ${j.config.tx}</div></div>
    <div class="card"><h3>Link</h3><div>Baud ${j.config.baud}</div><div>Rate ${j.config.rate_ms} ms</div></div>`;
  // sync source dropdown
  sourceSel.value = j.config.source.toLowerCase();
  const rows = document.getElementById('satRows');
  rows.innerHTML = '';
  const bars = document.getElementById('bars');
  bars.innerHTML='';
  j.sats.forEach(s=>{
    const tr = document.createElement('tr');
    tr.innerHTML = `<td>${s.sys}</td><td>${s.svid}</td><td>${s.cn0}</td><td>${qual(s.cn0)}</td><td>${s.el}</td><td>${s.az}</td><td>${s.used?'✔':'–'}</td>`;
    rows.appendChild(tr);
    const bar = document.createElement('div');
    bar.innerHTML = `<div style="display:flex;justify-content:space-between;font-size:12px;"><span>${s.sys}${s.svid}</span><span>${s.cn0} dBHz (${qual(s.cn0)})</span></div><div class="bar" style="width:${Math.min(s.cn0*2,100)}%"></div>`;
    bars.appendChild(bar);
  });
  logBox.textContent = j.log.join('\n');
});}
function sendCmd(action,val){fetch(`/api/cmd?action=${encodeURIComponent(action)}&value=${encodeURIComponent(val||'')}`)
  .then(r=>r.json()).then(j=>{if(!j.ok) alert('Failed: '+j.msg); update();});}
setInterval(update, 1000);
update();
</script>
</body>
</html>
)HTML";

static const char *sys_name(gnss_system_t s) {
    switch (s) {
    case GNSS_SYS_GPS: return "GPS";
    case GNSS_SYS_BDS: return "BDS";
    case GNSS_SYS_GLONASS: return "GLN";
    case GNSS_SYS_GALILEO: return "GAL";
    case GNSS_SYS_QZSS: return "QZ";
    case GNSS_SYS_SBAS: return "SB";
    default: return "UNK";
    }
}

static String antenna_txt(gps_ant_status_t st) {
    switch (st) {
    case GPS_ANT_OK: return "OK";
    case GPS_ANT_OPEN: return "OPEN";
    case GPS_ANT_SHORT: return "SHORT";
    default: return "?";
    }
}

static void handle_status(AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(12288);
    gps_source_t src = gps_provider_current_source();
    const gps_fix_t &fix = gps_provider_get_fix();
    const gnss_sat_view_t &view = gps_provider_get_sat_view();

    doc["source"] = (src == GPS_SOURCE_CASIC) ? "casic" : "legacy";
    JsonObject f = doc["fix"].to<JsonObject>();
    f["valid"] = fix.fix_valid;
    f["mode"] = fix.fix_type;
    f["lat"] = fix.lat_deg;
    f["lon"] = fix.lon_deg;
    f["alt"] = fix.alt_m;
    f["speed_kph"] = fix.speed_kph;
    f["course_deg"] = fix.course_deg;
    f["sats_used"] = fix.sats_used;
    f["hdop"] = fix.hdop;
    f["pdop"] = fix.pdop;
    f["vdop"] = fix.vdop;

    JsonObject sys = doc["sys"].to<JsonObject>();
    uint8_t sysCount[7] = {0};
    for (uint8_t i = 0; i < view.count; ++i) {
        gnss_system_t s = view.sats[i].system;
        if (s >= 0 && s < 7) sysCount[s]++;
    }
    sys["GPS"] = sysCount[GNSS_SYS_GPS];
    sys["BDS"] = sysCount[GNSS_SYS_BDS];
    sys["GLN"] = sysCount[GNSS_SYS_GLONASS];
    sys["GAL"] = sysCount[GNSS_SYS_GALILEO];
    sys["QZ"] = sysCount[GNSS_SYS_QZSS];
    sys["SB"] = sysCount[GNSS_SYS_SBAS];

    doc["antenna"] = antenna_txt(gps_provider_get_antenna_status());
    doc["log_enabled"] = gps_casic_status_log_enabled();
    JsonObject cfg = doc["config"].to<JsonObject>();
    cfg["source"] = (src == GPS_SOURCE_CASIC) ? "CASIC" : "LEGACY";
    cfg["baud"] = bruceConfig.gpsBaudrate;
    cfg["rate_ms"] = bruceConfig.gpsUpdateRateMs;
    cfg["rx"] = bruceConfigPins.gps_bus.rx;
    cfg["tx"] = bruceConfigPins.gps_bus.tx;

    JsonObject t = doc["time"].to<JsonObject>();
    bool tvalid = fix.hour < 24 && fix.min < 60 && fix.sec < 60 && fix.year > 0;
    t["valid"] = tvalid;
    t["hh"] = fix.hour;
    t["mm"] = fix.min;
    t["ss"] = fix.sec;
    t["day"] = fix.day;
    t["mon"] = fix.month;
    t["year"] = fix.year;

    JsonArray sats = doc["sats"].to<JsonArray>();
    for (uint8_t i = 0; i < view.count; ++i) {
        const auto &s = view.sats[i];
        JsonObject o = sats.add<JsonObject>();
        o["sys"] = sys_name(s.system);
        o["svid"] = s.svid;
        o["cn0"] = s.cn0_dbhz;
        o["el"] = s.elevation_deg;
        o["az"] = s.azimuth_deg;
        o["used"] = s.used_in_fix;
    }

    std::vector<String> lines;
    gps_casic_get_recent_nmea(lines, 80);
    JsonArray logArr = doc["log"].to<JsonArray>();
    for (const auto &ln : lines) logArr.add(ln);

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

static bool handle_command(AsyncWebServerRequest *request) {
    String action = request->arg("action");
    String value = request->arg("value");
    action.toLowerCase();
    bool ok = true;
    String msg = "ok";
    auto ensure_casic = []() {
        if (bruceConfig.gpsSource != BruceConfig::GPS_SOURCE_CASIC) {
            bruceConfig.setGpsSource(BruceConfig::GPS_SOURCE_CASIC);
            gps_provider_begin();
        }
    };

    if (action == "source") {
        value.toLowerCase();
        if (value == "casic") bruceConfig.setGpsSource(BruceConfig::GPS_SOURCE_CASIC);
        else bruceConfig.setGpsSource(BruceConfig::GPS_SOURCE_LEGACY);
        gps_provider_begin(); // re-init backend
    } else if (action == "baud") {
        ensure_casic();
        int baud = value.toInt();
        bruceConfig.setGpsBaudrate(baud);
        int applied = baud;
        ok = gps_casic_set_baudrate(gps_casic_baud_code_from_value(baud), applied);
        if (!ok) msg = "baud failed";
    } else if (action == "rate") {
        ensure_casic();
        uint16_t ms = static_cast<uint16_t>(value.toInt());
        bruceConfig.setGpsUpdateRate(ms);
        ok = gps_casic_set_update_rate_ms(ms);
        if (!ok) msg = "invalid rate";
    } else if (action == "system") {
        ensure_casic();
        ok = gps_casic_set_system_mode(static_cast<uint8_t>(value.toInt()));
        if (!ok) msg = "invalid system";
    } else if (action == "nmea") {
        ensure_casic();
        ok = gps_casic_set_nmea_version(static_cast<uint8_t>(value.toInt()));
        if (!ok) msg = "nmea failed";
    } else if (action == "reset") {
        ensure_casic();
        String v = value; v.toLowerCase();
        uint8_t mode = 0;
        if (v == "hot") mode = 0;
        else if (v == "warm") mode = 1;
        else if (v == "cold") mode = 2;
        else mode = 3;
        ok = gps_casic_reset(mode);
        if (!ok) msg = "reset failed";
    } else if (action == "save") {
        ensure_casic();
        ok = gps_casic_save_config();
        if (!ok) msg = "save failed";
    } else if (action == "log") {
        ensure_casic();
        if (value == "toggle") gps_casic_enable_status_log(!gps_casic_status_log_enabled());
        else {
            bool en = (value == "1" || value == "on" || value == "true");
            gps_casic_enable_status_log(en);
        }
    } else if (action == "info") {
        ensure_casic();
        gps_casic_info_t info{};
        ok = gps_casic_query_info(&info, 800);
        DynamicJsonDocument d(512);
        d["ok"] = ok;
        if (ok) {
            d["ma"] = info.manufacturer;
            d["ic"] = info.ic;
            d["sw"] = info.sw;
            d["build"] = info.build_time;
            d["mode"] = info.mode;
            d["cid"] = info.customer_id;
        } else d["msg"] = "info fail";
        String out; serializeJson(d, out);
        request->send(200, "application/json", out);
        return true;
    } else {
        ok = false;
        msg = "unknown action";
    }

    String out = String("{\"ok\":") + (ok ? "true" : "false") + ",\"msg\":\"" + msg + "\"}";
    request->send(ok ? 200 : 400, "application/json", out);
    return ok;
}

bool gps_web_start(uint16_t port) {
    if (running) return true;
    // Allow either STA connected or AP active
    wifi_mode_t mode = WiFi.getMode();
    bool staOk = WiFi.isConnected();
    bool apOk = mode & WIFI_MODE_AP;
    if (!staOk && !apOk) return false;
    staIp = WiFi.localIP();
    apIp = WiFi.softAPIP();
    listenPort = port;
    gpsServer = new AsyncWebServer(listenPort);

    gpsServer->on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/html", INDEX_HTML);
    });
    gpsServer->on("/api/status", HTTP_GET, handle_status);
    gpsServer->on("/api/cmd", HTTP_ANY, [](AsyncWebServerRequest *req) {
        handle_command(req);
    });

    gpsServer->begin();
    running = true;
    return true;
}

void gps_web_stop() {
    if (!running) return;
    gpsServer->end();
    delete gpsServer;
    gpsServer = nullptr;
    running = false;
}

bool gps_web_running() { return running; }

String gps_web_url() {
    if (!running) return "";
    String url;
    if (WiFi.isConnected() && staIp != INADDR_NONE && staIp != IPAddress((uint32_t)0)) {
        url = "http://" + staIp.toString() + ":" + String(listenPort) + "/";
    }
    if (url.length() == 0 && (WiFi.getMode() & WIFI_MODE_AP)) {
        url = "http://" + WiFi.softAPIP().toString() + ":" + String(listenPort) + "/";
    }
    return url;
}
