#include "companion.h"
#include "FramingSerialDevice.h"
#include "core/display.h"
#include "core/serial_commands/cli.h"
#include "core/sd_functions.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <Esp.h>
#include <FS.h>
#include <WiFi.h>
#include <esp_random.h>
#include <globals.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <modules/NRF24/nrf_common.h>
#include <modules/rf/rf_utils.h>

#ifndef BRUCE_VERSION
#define BRUCE_VERSION "dev"
#endif

#if !defined(LITE_VERSION)
// Defined in settings.cpp. Forward-declared here to avoid including the
// widely-included settings.h (keeps incremental rebuilds small).
void enableBLEAPI();
bool isBLEAPIEnabled();
#endif

namespace {

FramingSerialDevice g_framing(nullptr);
bool g_authed = false;

// --- challenge-response auth state ---
// When a token is configured, HELLO issues a random nonce; the host must answer
// with AUTH resp=sha256(token ":" nonceHex). The token itself never crosses the
// link (BLE is unencrypted). Nonce is one-shot. Auth is reset on BLE disconnect.
uint8_t g_nonce[16];
bool g_haveNonce = false;

// --- async streaming state (drained in companion::tick, serial-task context) ---
bool g_streaming = false;
uint32_t g_streamId = 0;
uint32_t g_streamSeq = 0;
uint32_t g_streamLastMs = 0;
uint32_t g_streamInterval = 1000;
String g_streamKind = "";
const char *g_radioOwner = "none"; // none | ui | companion
bool g_nrfReady = false;           // NRF24 powered up for an nrf stream
bool g_rfReady = false;            // CC1101 powered up for an rf stream
float g_rfStart = 433.0f, g_rfStop = 434.8f; // sub-GHz sweep band (MHz)

String buildCaps() {
    String c = "wifi,rf,ir,nrf,gpio,crypto,storage,status,gps,util,settings,power";
#ifdef USB_as_HID
    c += ",badusb";
#endif
#ifndef LITE_VERSION
    c += ",js";
#endif
#ifdef HAS_SCREEN
    c += ",screen";
#endif
#if defined(HAS_NS4168_SPKR) || defined(BUZZ_PIN)
    c += ",sound";
#endif
    return c;
}

// Extract value of "key=" from a payload, reading until the next space.
String fieldValue(const String &payload, const String &key) {
    int i = payload.indexOf(key + "=");
    if (i < 0) return "";
    i += key.length() + 1;
    int e = payload.indexOf(' ', i);
    if (e < 0) e = payload.length();
    return payload.substring(i, e);
}

// Emit one raw frame line to the real transport (the current global device).
void emit(const String &frame) {
    if (serialDevice) serialDevice->println(frame);
}

String toHex(const uint8_t *d, size_t n) {
    static const char *h = "0123456789abcdef";
    String s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += h[d[i] >> 4];
        s += h[d[i] & 0xf];
    }
    return s;
}

String sha256Hex(const String &in) {
    uint8_t h[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, (const uint8_t *)in.c_str(), in.length());
    mbedtls_sha256_finish(&c, h);
    mbedtls_sha256_free(&c);
    return toHex(h, 32);
}

// Path = text up to the first " size=" / " sha256=" / " chunk=" marker (so
// paths may contain spaces, e.g. "System Volume Information").
String extractPath(const String &rest) {
    int cut = rest.length();
    const char *keys[] = {" size=", " sha256=", " chunk="};
    for (const char *k : keys) {
        int i = rest.indexOf(k);
        if (i >= 0 && i < cut) cut = i;
    }
    String p = rest.substring(0, cut);
    p.trim();
    return p;
}

int clampChunk(const String &rest) {
    int chunk = 512;
    String cv = fieldValue(rest, "chunk");
    if (cv.length()) chunk = cv.toInt();
    if (chunk < 16) chunk = 16;
    if (chunk > 1024) chunk = 1024;
    return chunk;
}

// ---- file get (device -> host): metadata RSP, EVT chunks (base64), sha256 ----
void doFileGet(uint32_t id, const String &rest) {
    int chunk = clampChunk(rest);
    String path = extractPath(rest);
    FS *fs;
    if (!getFsStorage(fs) || !fs->exists(path)) {
        emit("ERR " + String(id) + " 4 no such file");
        return;
    }
    File f = fs->open(path);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        emit("ERR " + String(id) + " 4 cannot open");
        return;
    }
    size_t size = f.size();
    size_t nchunks = (size + chunk - 1) / chunk;
    emit("RSP " + String(id) + " size=" + String((uint32_t)size) + " chunks=" +
         String((uint32_t)nchunks) + " chunk_size=" + String(chunk));

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    size_t b64cap = 4 * ((chunk + 2) / 3) + 1;
    uint8_t *buf = (uint8_t *)malloc(chunk);
    uint8_t *b64 = (uint8_t *)malloc(b64cap);
    if (!buf || !b64) {
        free(buf);
        free(b64);
        f.close();
        mbedtls_sha256_free(&ctx);
        emit("ERR " + String(id) + " 4 oom");
        return;
    }
    uint32_t n = 0;
    int r;
    while ((r = f.read(buf, chunk)) > 0) {
        mbedtls_sha256_update(&ctx, buf, r);
        size_t olen = 0;
        mbedtls_base64_encode(b64, b64cap, &olen, buf, r);
        String s;
        s.reserve(olen);
        for (size_t i = 0; i < olen; i++) s += (char)b64[i];
        emit("EVT " + String(id) + " chunk " + String(n) + " " + s);
        n++;
    }
    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    free(buf);
    free(b64);
    f.close();
    emit("RSP " + String(id) + " sha256=" + toHex(hash, 32));
    emit("END " + String(id) + " 0");
}

// ---- file put (host -> device): session + acked chunks + sha256 verify ----
struct PutSession {
    bool active = false;
    File f;
    String expSha;
    size_t written = 0;
    mbedtls_sha256_context ctx;
} g_put;

void doFilePutStart(uint32_t id, const String &rest) {
    if (g_put.active) {
        emit("ERR " + String(id) + " 2 put in progress");
        return;
    }
    int chunk = clampChunk(rest);
    String path = extractPath(rest);
    FS *fs;
    if (!getFsStorage(fs)) {
        emit("ERR " + String(id) + " 4 no fs");
        return;
    }
    g_put.f = fs->open(path, FILE_WRITE, true);
    if (!g_put.f) {
        emit("ERR " + String(id) + " 4 cannot create");
        return;
    }
    g_put.active = true;
    g_put.expSha = fieldValue(rest, "sha256");
    g_put.written = 0;
    mbedtls_sha256_init(&g_put.ctx);
    mbedtls_sha256_starts(&g_put.ctx, 0);
    emit("RSP " + String(id) + " ready chunk_size=" + String(chunk));
    emit("END " + String(id) + " 0");
}

void doFilePutChunk(uint32_t id, const String &rest) {
    if (!g_put.active) {
        emit("ERR " + String(id) + " 4 no put session");
        return;
    }
    int sp = rest.indexOf(' '); // skip the <n> index
    String b64 = (sp < 0) ? String("") : rest.substring(sp + 1);
    b64.trim();
    size_t dcap = (b64.length() / 4 + 1) * 3 + 4;
    uint8_t *dst = (uint8_t *)malloc(dcap);
    if (!dst) {
        emit("ERR " + String(id) + " 4 oom");
        return;
    }
    size_t olen = 0;
    int rc = mbedtls_base64_decode(dst, dcap, &olen, (const uint8_t *)b64.c_str(), b64.length());
    if (rc != 0) {
        free(dst);
        emit("ERR " + String(id) + " 6 bad base64");
        return;
    }
    g_put.f.write(dst, olen);
    mbedtls_sha256_update(&g_put.ctx, dst, olen);
    g_put.written += olen;
    free(dst);
    emit("END " + String(id) + " 0"); // ack
}

void doFilePutEnd(uint32_t id) {
    if (!g_put.active) {
        emit("ERR " + String(id) + " 4 no put session");
        return;
    }
    g_put.f.close();
    uint8_t hash[32];
    mbedtls_sha256_finish(&g_put.ctx, hash);
    mbedtls_sha256_free(&g_put.ctx);
    String got = toHex(hash, 32);
    bool ok = (g_put.expSha.length() == 0) || got.equalsIgnoreCase(g_put.expSha);
    g_put.active = false;
    emit("RSP " + String(id) + " written=" + String((uint32_t)g_put.written) + " sha256=" + got +
         " ok=" + String(ok ? "true" : "false"));
    emit("END " + String(id) + " " + String(ok ? 0 : 4));
}

// ---- real radio stream kinds (wifi / nrf) -------------------------------------

const char *wifiEncStr(int enc) {
    switch (enc) {
        case WIFI_AUTH_OPEN: return "open";
        case WIFI_AUTH_WEP: return "wep";
        case WIFI_AUTH_WPA_PSK: return "wpa";
        case WIFI_AUTH_WPA2_PSK: return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-ent";
#ifdef WIFI_AUTH_WPA3_PSK
        case WIFI_AUTH_WPA3_PSK: return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
#endif
        default: return "?";
    }
}

// WiFi stream uses the async scan API so the serial task never blocks: kick a
// scan, poll scanComplete() each tick, emit one EVT per network when ready,
// then idle until the interval elapses before re-scanning. SPI-safe (WiFi has
// its own radio peripheral; no shared bus with the display).
void tickWifi(uint32_t now) {
    int16_t st = WiFi.scanComplete();
    if (st == WIFI_SCAN_RUNNING) return;
    if (st >= 0) {
        emit("EVT " + String(g_streamId) + " wifi seq=" + String(g_streamSeq) + " count=" +
             String(st) + " ms=" + String(now));
        int n = st > 40 ? 40 : st; // cap frames per sweep
        for (int i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            ssid.replace('\r', ' ');
            ssid.replace('\n', ' '); // never break framing
            emit("EVT " + String(g_streamId) + " wifi net ch=" + String(WiFi.channel(i)) + " rssi=" +
                 String(WiFi.RSSI(i)) + " enc=" + String(wifiEncStr(WiFi.encryptionType(i))) +
                 " bssid=" + WiFi.BSSIDstr(i) + " ssid=" + (ssid.length() ? ssid : String("<hidden>")));
        }
        WiFi.scanDelete();
        g_streamSeq++;
        g_streamLastMs = now; // hold off the next scan until the interval passes
        return;
    }
    // st == WIFI_SCAN_FAILED (-2): idle -> (re)start a scan when due
    if (g_streamLastMs == 0 || (now - g_streamLastMs) >= g_streamInterval) {
        if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
        WiFi.scanNetworks(true); // async (non-blocking)
        g_streamLastMs = now;
    }
}

bool nrfStreamBegin() {
    if (!nrf_start(NRF_MODE_SPI)) return false;
    NRFradio.setAutoAck(false);
    NRFradio.disableCRC();       // accept any carrier
    NRFradio.setAddressWidth(2); // reverse-engineering tactic (see nrf_spectrum)
    const uint8_t noise[][2] = {{0x55, 0x55}, {0xAA, 0xAA}, {0xA0, 0xAA},
                                {0xAB, 0xAA}, {0xAC, 0xAA}, {0xAD, 0xAA}};
    for (uint8_t i = 0; i < 6; ++i) NRFradio.openReadingPipe(i, noise[i]);
    NRFradio.setDataRate(RF24_1MBPS);
    g_nrfReady = true;
    return true;
}

// One RPD spectrum sweep over the 2.4 GHz band (80 channels), a few samples per
// channel. Fast (~tens of ms). NOTE: on T-Embed the NRF24 shares the TFT SPI
// bus, so keep the device screen idle while streaming nrf (no SPI mutex exists).
void emitNrfSweep(uint32_t now) {
    const int CH = 80;
    const int SAMPLES = 3;
    String active;
    int peakCh = -1, peakHits = 0, total = 0;
    for (int c = 0; c < CH; c++) {
        NRFradio.setChannel(c);
        int hits = 0;
        for (int s = 0; s < SAMPLES; s++) {
            NRFradio.startListening();
            delayMicroseconds(130);
            NRFradio.stopListening();
            if (NRFradio.testRPD()) hits++;
        }
        if (hits > 0) {
            if (active.length()) active += ",";
            active += String(c) + ":" + String(hits);
            total++;
            if (hits > peakHits) {
                peakHits = hits;
                peakCh = c;
            }
        }
    }
    emit("EVT " + String(g_streamId) + " nrf seq=" + String(g_streamSeq) + " ms=" + String(now) +
         " channels=" + String(CH) + " active_n=" + String(total) + " peak_ch=" + String(peakCh) +
         " peak=" + String(peakHits) + " active=" + (active.length() ? active : String("none")));
}

bool rfStreamBegin(float start, float stop) {
    // Validate against the CC1101 bands; fall back to the 433 ISM band.
    auto ok = [](float f) {
        return (f >= 280 && f <= 350) || (f >= 387 && f <= 468) || (f >= 779 && f <= 928);
    };
    if (!ok(start) || !ok(stop) || stop <= start) {
        start = 433.0f;
        stop = 434.8f;
    }
    g_rfStart = start;
    g_rfStop = stop;
    if (!initRfModule("rx", start)) return false;
    ELECHOUSE_cc1101.setRxBW(200); // narrow RxBW for cleaner RSSI (matches rf_waterfall)
    g_rfReady = true;
    return true;
}

// One sub-GHz RSSI sweep across [g_rfStart, g_rfStop] (40 bins). Mirrors the
// rf_waterfall arbitration (dummy TFT pixel) for the shared SPI bus. Keep the
// device screen idle while streaming rf.
void emitRfSweep(uint32_t now) {
    const int N = 40;
    float step = (g_rfStop - g_rfStart) / (float)(N - 1);
    String rssis;
    int peak = -200, peakIdx = 0, floor_ = 200;
    for (int i = 0; i < N; i++) {
        float f = g_rfStart + step * i;
        setMHZ(f);
        ELECHOUSE_cc1101.SetRx();            // re-strobe RX so the synth recalibrates
                                             // at the new freq and RSSI becomes valid
        tft.drawPixel(0, 0, 0);              // SPI arbitration on the shared bus
        vTaskDelay(2 / portTICK_PERIOD_MS);  // yield + let RSSI settle (avoids 0x80)
        int r = ELECHOUSE_cc1101.getRssi();
        tft.drawPixel(0, 0, 0);              // re-sync after the status read
        if (i) rssis += ",";
        rssis += String(r);
        if (r > peak) { peak = r; peakIdx = i; }
        if (r < floor_) floor_ = r;
    }
    float peakF = g_rfStart + step * peakIdx;
    emit("EVT " + String(g_streamId) + " rf seq=" + String(g_streamSeq) + " ms=" + String(now) +
         " f0=" + String(g_rfStart, 2) + " f1=" + String(g_rfStop, 2) + " step=" + String(step, 3) +
         " n=" + String(N) + " peak_f=" + String(peakF, 2) + " peak=" + String(peak) +
         " floor=" + String(floor_) + " rssi=" + rssis);
}

void streamTeardown() {
    if (g_streamKind == "wifi") {
        int16_t st = WiFi.scanComplete();
        if (st >= 0) WiFi.scanDelete();
    }
    if (g_nrfReady) {
        NRFradio.stopListening();
        NRFradio.powerDown();
        g_nrfReady = false;
    }
    if (g_rfReady) {
        deinitRfModule();
        g_rfReady = false;
    }
}

} // namespace

namespace companion {

bool looksLikeFrame(const String &line) {
    return line.startsWith("REQ ") || line.startsWith("ACK ");
}

// Drop the authenticated session (call on BLE disconnect so the next central
// must re-authenticate; otherwise it would inherit the previous auth state).
void resetAuth() {
    g_authed = false;
    g_haveNonce = false;
    streamTeardown(); // release wifi/nrf radios if a stream was active
    g_streaming = false;
    g_radioOwner = "none";
}

void handleLine(SerialCli &cli, const String &raw) {
    String line = raw;
    line.trim();

    int s1 = line.indexOf(' ');
    if (s1 < 0) {
        emit("ERR 0 6 BADFRAME");
        return;
    }
    String type = line.substring(0, s1);
    String rest = line.substring(s1 + 1);
    rest.trim();

    int s2 = rest.indexOf(' ');
    String idStr = (s2 < 0) ? rest : rest.substring(0, s2);
    String payload = (s2 < 0) ? String("") : rest.substring(s2 + 1);
    payload.trim();
    uint32_t id = (uint32_t)idStr.toInt();

    if (type == "ACK") return; // host flow-control, ignored in v1
    if (type != "REQ") {
        emit("ERR " + String(id) + " 6 BADFRAME");
        return;
    }

    // --- HELLO / authentication (challenge-response) ---
    if (payload.startsWith("HELLO")) {
        bool overBle = false;
#if !defined(LITE_VERSION)
        overBle = isBLEAPIEnabled();
#endif
        const String &tokenCfg = bruceConfig.companionToken;
        String helloInfo = "fw=Treecataje/" + String(BRUCE_VERSION) +
                           " proto=1 board=T_EMBED_CC1101 mtu=512 name=Bruc";

        if (tokenCfg.length() == 0) {
            // Open mode: only over the wired USB link. Over the air (BLE) a
            // token MUST be configured first — this is the radio-control lock.
            if (overBle) {
                emit("ERR " + String(id) + " 7 AUTH token-required-for-ble");
                return;
            }
            g_authed = true;
            g_haveNonce = false;
            emit("RSP " + String(id) + " " + helloInfo + " auth=open");
            emit("RSP " + String(id) + " caps=" + buildCaps());
            emit("END " + String(id) + " 0");
            return;
        }

        // Token configured: issue a fresh one-shot challenge; the host must
        // follow with AUTH resp=sha256(token ":" nonce). Not authed yet.
        esp_fill_random(g_nonce, sizeof(g_nonce));
        g_haveNonce = true;
        g_authed = false;
        emit("RSP " + String(id) + " " + helloInfo + " auth=required");
        emit("RSP " + String(id) + " nonce=" + toHex(g_nonce, sizeof(g_nonce)));
        emit("END " + String(id) + " 0");
        return;
    }

    // --- AUTH: answer to the HELLO challenge ---
    if (payload.startsWith("AUTH")) {
        if (!g_haveNonce) {
            emit("ERR " + String(id) + " 7 AUTH no-challenge");
            return;
        }
        String resp = fieldValue(payload, "resp");
        String expect = sha256Hex(bruceConfig.companionToken + ":" + toHex(g_nonce, sizeof(g_nonce)));
        g_haveNonce = false; // one-shot, even on failure (forces a new HELLO)
        if (resp.length() && resp.equalsIgnoreCase(expect)) {
            g_authed = true;
            emit("RSP " + String(id) + " ok auth=ok");
            emit("RSP " + String(id) + " caps=" + buildCaps());
            emit("END " + String(id) + " 0");
        } else {
            g_authed = false;
            emit("ERR " + String(id) + " 7 AUTH");
        }
        return;
    }

    if (!g_authed) {
        emit("ERR " + String(id) + " 7 AUTH");
        return;
    }

    // --- token management (only over an already-authed session) ---
    if (payload.startsWith("companion token set ")) {
        String t = payload.substring(20);
        t.trim();
        bruceConfig.companionToken = t;
        bruceConfig.saveFile();
        emit("RSP " + String(id) + " token=set len=" + String(t.length()));
        emit("END " + String(id) + " 0");
        return;
    }
    if (payload == "companion token clear") {
        bruceConfig.companionToken = "";
        bruceConfig.saveFile();
        emit("RSP " + String(id) + " token=cleared");
        emit("END " + String(id) + " 0");
        return;
    }
    if (payload == "companion token status") {
        emit("RSP " + String(id) + " token_set=" +
             String(bruceConfig.companionToken.length() ? "true" : "false"));
        emit("END " + String(id) + " 0");
        return;
    }

    // --- companion-specific verbs ---
    if (payload == "companion caps") {
        emit("RSP " + String(id) + " caps=" + buildCaps());
        emit("END " + String(id) + " 0");
        return;
    }
    if (payload == "companion busy") {
        emit("RSP " + String(id) + " owner=" + String(g_radioOwner) +
             (g_streaming ? " stream=" + g_streamKind + " id=" + String(g_streamId) : ""));
        emit("END " + String(id) + " 0");
        return;
    }
    // --- streaming: start/stop async EVT (drained in tick()) ---
    if (payload.startsWith("companion stream start")) {
        String spec = payload.substring(String("companion stream start").length());
        spec.trim();
        if (spec.length() == 0) spec = "telemetry";
        if (g_streaming) {
            emit("ERR " + String(id) + " 2 stream already active id=" + String(g_streamId));
            return;
        }
        // base kind = first token; the rest carries optional args (rf range,
        // interval=<ms>).
        String kind = spec;
        String rest = "";
        int sp = spec.indexOf(' ');
        if (sp > 0) {
            kind = spec.substring(0, sp);
            rest = spec.substring(sp + 1);
            rest.trim();
        }
        if (kind != "telemetry" && kind != "wifi" && kind != "nrf" && kind != "rf") {
            emit("ERR " + String(id) + " 3 unknown stream kind=" + kind);
            return;
        }
        // optional interval override (interval=<ms>), else default 1 Hz
        g_streamInterval = 1000;
        String iv = fieldValue(spec, "interval");
        if (iv.length()) {
            long v = iv.toInt();
            if (v < 200) v = 200;
            if (v > 10000) v = 10000;
            g_streamInterval = (uint32_t)v;
        }
        // Per-kind radio init (must succeed before we mark the stream active).
        if (kind == "wifi") {
            if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
        } else if (kind == "nrf") {
            if (!nrfStreamBegin()) {
                emit("ERR " + String(id) + " 4 nrf init failed (radio not found?)");
                return;
            }
        } else if (kind == "rf") {
            float a = g_rfStart, b = g_rfStop;
            int s2 = rest.indexOf(' ');
            if (s2 > 0) {  // "rf <start> <stop>"
                a = rest.substring(0, s2).toFloat();
                b = rest.substring(s2 + 1).toFloat();
            }
            if (!rfStreamBegin(a, b)) {
                emit("ERR " + String(id) + " 4 rf init failed (CC1101 not found?)");
                return;
            }
        }
        g_streaming = true;
        g_streamId = id;
        g_streamSeq = 0;
        g_streamKind = kind;
        g_streamLastMs = 0; // emit first tick immediately
        g_radioOwner = "companion";
        emit("RSP " + String(id) + " streaming=" + kind + " id=" + String(id) +
             " interval=" + String(g_streamInterval) +
             (kind == "rf" ? " band=" + String(g_rfStart, 2) + "-" + String(g_rfStop, 2) : ""));
        emit("END " + String(id) + " 0");
        return;
    }
    if (payload.startsWith("companion stream stop")) {
        streamTeardown();
        g_streaming = false;
        g_radioOwner = "none";
        emit("RSP " + String(id) + " stopped=" + String(g_streamId));
        emit("END " + String(id) + " 0");
        return;
    }
    if (payload == "companion ping") {
        emit("RSP " + String(id) + " pong");
        emit("END " + String(id) + " 0");
        return;
    }
#if !defined(LITE_VERSION)
    // Enable/disable the BLE API remotely. IMPORTANT: respond on the CURRENT
    // transport BEFORE toggling, because enableBLEAPI() switches the global
    // serialDevice (USB <-> BLE).
    if (payload.startsWith("companion ble")) {
        String arg = payload.substring(String("companion ble").length());
        arg.trim();
        bool on = isBLEAPIEnabled();
        if (arg == "on") {
            if (on) {
                emit("RSP " + String(id) + " ble=on already name=Bruc");
                emit("END " + String(id) + " 0");
            } else {
                emit("RSP " + String(id) + " ble=on name=Bruc");
                emit("END " + String(id) + " 0");
                enableBLEAPI(); // serialDevice -> BLE after we replied on USB
            }
        } else if (arg == "off") {
            if (!on) {
                emit("RSP " + String(id) + " ble=off already");
                emit("END " + String(id) + " 0");
            } else {
                emit("RSP " + String(id) + " ble=off");
                emit("END " + String(id) + " 0");
                enableBLEAPI(); // serialDevice -> USB after we replied on BLE
            }
        } else { // status
            emit("RSP " + String(id) + " ble=" + String(on ? "on" : "off"));
            emit("END " + String(id) + " 0");
        }
        return;
    }
#endif
    // file transfer (chunked base64 + sha256). Check putchunk/putend before
    // "put " (no trailing-space collision, but explicit is safer).
    if (payload.startsWith("companion file get ")) {
        doFileGet(id, payload.substring(19));
        return;
    }
    if (payload.startsWith("companion file putchunk ")) {
        doFilePutChunk(id, payload.substring(24));
        return;
    }
    if (payload == "companion file putend" || payload.startsWith("companion file putend ")) {
        doFilePutEnd(id);
        return;
    }
    if (payload.startsWith("companion file put ")) {
        doFilePutStart(id, payload.substring(19));
        return;
    }

    if (payload.startsWith("companion ")) {
        emit("ERR " + String(id) + " 3 UNSUPPORTED"); // stream/events: Phase 3b
        return;
    }

    // --- generic: run an existing CLI command, framed, NO backToMenu ---
    SerialDevice *prev = serialDevice;
    g_framing.setInner(prev);
    g_framing.beginRequest(id);
    serialDevice = &g_framing;
    bool okCmd = cli.parse(payload);
    serialDevice = prev;
    g_framing.endRequest(okCmd ? 0 : 1);
}

void tick() {
    if (!g_streaming) return;
    uint32_t now = millis();
    // WiFi manages its own cadence (async scan): poll every tick.
    if (g_streamKind == "wifi") {
        tickWifi(now);
        return;
    }
    if (g_streamLastMs != 0 && (now - g_streamLastMs) < g_streamInterval) return;
    g_streamLastMs = now;
    if (g_streamKind == "nrf") {
        emitNrfSweep(now);
        g_streamSeq++;
        return;
    }
    if (g_streamKind == "rf") {
        emitRfSweep(now);
        g_streamSeq++;
        return;
    }
    // "telemetry": live device vitals (ms / free heap).
    emit("EVT " + String(g_streamId) + " tick seq=" + String(g_streamSeq) + " ms=" +
         String(now) + " heap=" + String((uint32_t)ESP.getFreeHeap()));
    g_streamSeq++;
}

} // namespace companion
