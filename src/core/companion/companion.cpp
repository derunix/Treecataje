#include "companion.h"
#include "FramingSerialDevice.h"
#include "core/serial_commands/cli.h"
#include "core/sd_functions.h"
#include <FS.h>
#include <globals.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

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

} // namespace

namespace companion {

bool looksLikeFrame(const String &line) {
    return line.startsWith("REQ ") || line.startsWith("ACK ");
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

    // --- HELLO / authentication ---
    if (payload.startsWith("HELLO")) {
        String token = fieldValue(payload, "token");
        bool ok = (bruceConfig.companionToken.length() == 0) ||
                  (token == bruceConfig.companionToken);
        if (!ok) {
            emit("ERR " + String(id) + " 7 AUTH");
            return;
        }
        g_authed = true;
        emit("RSP " + String(id) + " fw=Treecataje/" + String(BRUCE_VERSION) +
             " proto=1 board=T_EMBED_CC1101 mtu=512 name=Bruc");
        emit("RSP " + String(id) + " caps=" + buildCaps());
        emit("END " + String(id) + " 0");
        return;
    }

    if (!g_authed) {
        emit("ERR " + String(id) + " 7 AUTH");
        return;
    }

    // --- companion-specific verbs ---
    if (payload == "companion caps") {
        emit("RSP " + String(id) + " caps=" + buildCaps());
        emit("END " + String(id) + " 0");
        return;
    }
    if (payload == "companion busy") {
        emit("RSP " + String(id) + " owner=none"); // busy-flag infra: Phase 3
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

} // namespace companion
