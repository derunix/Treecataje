#include "nrf_sniffer.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "nrf_common.h"
#include <globals.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace {
String addrToString(const uint8_t a[5]) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X", a[0], a[1], a[2], a[3], a[4]);
    return String(buf);
}

std::pair<String, String> guessVendor(const uint8_t a[5]) {
    struct Prefix {
        uint8_t p0;
        uint8_t p1;
        const char *vendor;
        const char *type;
    };
    static const Prefix prefixes[] = {
        {0xE7, 0xE7, "Nordic (default)", "Dev/Examples"},
        {0xC2, 0xC2, "Logitech/ESB (guess)", "HID Dongle"},
        {0xAA, 0xAA, "Toy/Clone", "SimpleTX"},
        {0x55, 0x55, "Generic", "Unknown"},
    };
    for (const auto &p : prefixes) {
        if (a[0] == p.p0 && a[1] == p.p1) return {p.vendor, p.type};
    }
    return {"Unknown", "Unknown"};
}

void addOrUpdate(
    std::vector<NrfSniffedDevice> &devices, const uint8_t fullAddr[5], uint8_t channel, uint8_t pipe,
    const uint8_t *payload, uint8_t len
) {
    const auto vend = guessVendor(fullAddr);
    const uint32_t now = millis();

    auto it = std::find_if(devices.begin(), devices.end(), [&](const NrfSniffedDevice &d) {
        return memcmp(d.addr, fullAddr, 5) == 0;
    });
    if (it == devices.end()) {
        NrfSniffedDevice d{};
        memcpy(d.addr, fullAddr, 5);
        d.channel = channel;
        d.pipe = pipe;
        d.firstSeen = now;
        d.lastSeen = now;
        d.hits = 1;
        d.sampleLen = len;
        if (len > 0 && len <= 32 && payload) memcpy(d.sample, payload, len);
        d.vendor = vend.first;
        d.type = vend.second;
        devices.push_back(d);
    } else {
        it->hits++;
        it->lastSeen = now;
        it->channel = channel;
        it->pipe = pipe;
        if (it->sampleLen == 0 && len > 0 && len <= 32 && payload) {
            it->sampleLen = len;
            memcpy(it->sample, payload, len);
        }
        if (it->vendor == "Unknown") {
            it->vendor = vend.first;
            it->type = vend.second;
        }
    }
}

void renderUI(const std::vector<NrfSniffedDevice> &devices, uint8_t channel) {
    drawMainBorder();
    tft.setCursor(10, 20);
    tft.setTextSize(FM);
    tft.println("NRF24 Sniffer");
    tft.setTextSize(FP);
    tft.printf("Scanning CH %u   Found: %u\n", channel, (unsigned)devices.size());
    tft.println("ESC to exit");

    // Show top devices by hit count
    std::vector<NrfSniffedDevice> sorted = devices;
    std::sort(sorted.begin(), sorted.end(), [](const NrfSniffedDevice &a, const NrfSniffedDevice &b) {
        return a.hits > b.hits;
    });

    int y = 70;
    const int maxRows = 6;
    for (int i = 0; i < (int)sorted.size() && i < maxRows; ++i) {
        const auto &d = sorted[i];
        tft.setCursor(10, y);
        tft.printf("CH%3u %s (%lu)", d.channel, addrToString(d.addr).c_str(), (unsigned long)d.hits);
        y += 14;
        tft.setCursor(10, y);
        tft.printf("%s / %s", d.vendor.c_str(), d.type.c_str());
        y += 16;
    }
}
} // namespace

static void configureSniffer() {
    NRFradio.setAutoAck(false);
    NRFradio.disableCRC();
    NRFradio.enableDynamicPayloads();
    NRFradio.setAddressWidth(2); // shorter AW lets us pull the remaining bytes from payload
    NRFradio.setPayloadSize(32);
    NRFradio.setPALevel(RF24_PA_MAX);
    NRFradio.setDataRate(RF24_1MBPS); // best compatibility with most ESB devices
    NRFradio.openReadingPipe(0, 0xFFFF);
    NRFradio.startListening();
}

static void collectDevices(std::vector<NrfSniffedDevice> &devices, uint32_t scanTimeMs, uint16_t dwellPerChannelMs) {
    if (!nrf_start()) {
        Serial.println("Fail Starting radio");
        return;
    }

    configureSniffer();
    uint8_t channel = 2;
    const unsigned long endAt = millis() + scanTimeMs;

    while ((long)(endAt - millis()) > 0) {
        NRFradio.setChannel(channel);
        unsigned long listenStart = millis();

        while (millis() - listenStart < dwellPerChannelMs) {
            uint8_t pipe = 0;
            while (NRFradio.available(&pipe)) {
                uint8_t len = NRFradio.getDynamicPayloadSize();
                if (len < 3 || len > 32) {
                    NRFradio.flush_rx();
                    break;
                }

                uint8_t payload[32];
                NRFradio.read(payload, len);

                uint8_t fullAddr[5] = {0xFF, 0xFF, payload[0], payload[1], payload[2]};
                addOrUpdate(devices, fullAddr, channel, pipe, payload, len);
            }
        }

        channel = (channel >= 125) ? 1 : channel + 1;
    }

    NRFradio.stopListening();
    NRFradio.powerDown();
}

std::vector<NrfSniffedDevice> nrf_sniffer_collect(uint32_t scanTimeMs, uint16_t dwellPerChannelMs) {
    std::vector<NrfSniffedDevice> devices;
    collectDevices(devices, scanTimeMs, dwellPerChannelMs);
    return devices;
}

void nrf_sniffer() {
    if (!nrf_start()) {
        Serial.println("Fail Starting radio");
        displayError("NRF24 not found");
        delay(500);
        return;
    }

    configureSniffer();

    std::vector<NrfSniffedDevice> devices;
    uint8_t channel = 2;
    unsigned long lastDraw = 0;

    renderUI(devices, channel);

    while (!check(EscPress)) {
        NRFradio.setChannel(channel);
        unsigned long listenStart = millis();

        while (millis() - listenStart < 40 && !check(EscPress)) {
            uint8_t pipe = 0;
            while (NRFradio.available(&pipe)) {
                uint8_t len = NRFradio.getDynamicPayloadSize();
                if (len < 3 || len > 32) {
                    NRFradio.flush_rx();
                    break;
                }

                uint8_t payload[32];
                NRFradio.read(payload, len);

                uint8_t fullAddr[5] = {0xFF, 0xFF, payload[0], payload[1], payload[2]};
                addOrUpdate(devices, fullAddr, channel, pipe, payload, len);
            }
        }

        channel++;
        if (channel > 125) channel = 1;

        if (millis() - lastDraw > 400) {
            renderUI(devices, channel);
            lastDraw = millis();
        }
    }

    NRFradio.stopListening();
    NRFradio.powerDown();
}
