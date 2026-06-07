#include "companion.h"
#include "FramingSerialDevice.h"
#include "core/serial_commands/cli.h"
#include <globals.h>

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
    if (payload.startsWith("companion ")) {
        emit("ERR " + String(id) + " 3 UNSUPPORTED"); // file/stream: Phase 3
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
