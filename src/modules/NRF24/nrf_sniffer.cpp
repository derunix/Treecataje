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

void nrf_packet_analyzer() {
    if (!nrf_start()) {
        Serial.println("Fail Starting radio for packet analyzer");
        displayError("NRF24 not found");
        delay(500);
        return;
    }

    // Get filesystem for logging
    FS *fs = nullptr;
    bool canLog = getFsStorage(fs) && fs;
    File logFile;

    if (canLog) {
        String filename = "/nrf_packets_" + String(millis()) + ".log";
        logFile = fs->open(filename.c_str(), FILE_WRITE);
        if (logFile) {
            logFile.println("=== NRF24 Packet Analyzer Log ===");
            logFile.printf("Started: %lu ms\n", millis());
            logFile.println("Format: [timestamp] CH:channel ADDR:address PIPE:pipe LEN:length DATA:hex [DECODED]");
            logFile.println("---");
            Serial.printf("[NRF] Logging to: %s\n", filename.c_str());
        }
    }

    configureSniffer();

    struct PacketCapture {
        uint32_t timestamp;
        uint8_t channel;
        uint8_t addr[5];
        uint8_t pipe;
        uint8_t len;
        uint8_t data[32];
    };

    std::vector<PacketCapture> recentPackets;
    const size_t maxRecentPackets = 50;

    uint8_t channel = 2;
    unsigned long lastDraw = 0;
    uint32_t totalPackets = 0;
    std::vector<NrfSniffedDevice> devices;

    drawMainBorder();
    tft.setCursor(10, 20);
    tft.setTextSize(FM);
    tft.println("NRF24 Packet Analyzer");
    tft.setTextSize(FP);
    tft.println("Capturing and logging...");
    tft.println("ESC to stop");

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

                // Full address extraction
                uint8_t fullAddr[5] = {0xFF, 0xFF, payload[0], payload[1], payload[2]};

                // Store packet
                PacketCapture cap;
                cap.timestamp = millis();
                cap.channel = channel;
                memcpy(cap.addr, fullAddr, 5);
                cap.pipe = pipe;
                cap.len = len;
                memcpy(cap.data, payload, len);

                recentPackets.push_back(cap);
                if (recentPackets.size() > maxRecentPackets) {
                    recentPackets.erase(recentPackets.begin());
                }

                totalPackets++;

                // Update device list
                addOrUpdate(devices, fullAddr, channel, pipe, payload, len);

                // Decode packet type
                String decoded = "RAW";
                if (len >= 8) {
                    // Check if HID keyboard report (8 bytes)
                    if (payload[0] < 0x08 && payload[2] < 0xE8) {
                        decoded = "HID_KBD";
                        if (payload[2] != 0) {
                            decoded += " KEY:0x";
                            decoded += String(payload[2], HEX);
                        }
                        if (payload[0] != 0) {
                            decoded += " MOD:0x";
                            decoded += String(payload[0], HEX);
                        }
                    }
                    // Check if HID mouse report
                    else if (payload[0] < 0x08 && len == 8) {
                        decoded = "HID_MOUSE";
                        if (payload[0] != 0) decoded += " BTN:" + String(payload[0]);
                        if ((int8_t)payload[1] != 0 || (int8_t)payload[2] != 0) {
                            decoded += " X:" + String((int8_t)payload[1]);
                            decoded += " Y:" + String((int8_t)payload[2]);
                        }
                    }
                }

                // Log to file
                if (logFile) {
                    logFile.printf("[%lu] CH:%u ADDR:%02X%02X%02X%02X%02X PIPE:%u LEN:%u DATA:",
                        cap.timestamp, cap.channel,
                        cap.addr[0], cap.addr[1], cap.addr[2], cap.addr[3], cap.addr[4],
                        cap.pipe, cap.len);
                    for (uint8_t i = 0; i < cap.len; i++) {
                        logFile.printf("%02X", cap.data[i]);
                        if (i < cap.len - 1) logFile.print(" ");
                    }
                    logFile.printf(" [%s]\n", decoded.c_str());
                    logFile.flush();
                }

                // Log to serial
                Serial.printf("[NRF] CH:%u ADDR:%02X%02X%02X%02X%02X LEN:%u [%s]\n",
                    cap.channel,
                    cap.addr[0], cap.addr[1], cap.addr[2], cap.addr[3], cap.addr[4],
                    cap.len, decoded.c_str());
            }
        }

        channel++;
        if (channel > 125) channel = 1;

        // Update UI
        if (millis() - lastDraw > 500) {
            drawMainBorder();
            tft.setCursor(10, 20);
            tft.setTextSize(FM);
            tft.println("NRF24 Packet Analyzer");
            tft.setTextSize(FP);
            tft.printf("CH: %u  Total: %lu\n", channel, totalPackets);
            tft.printf("Devices: %u  Log: %s\n", devices.size(), canLog ? "YES" : "NO");
            tft.println("ESC to stop");
            tft.println("---");

            // Show recent packets
            int y = 80;
            int shown = 0;
            for (int i = recentPackets.size() - 1; i >= 0 && shown < 5; i--, shown++) {
                const auto &p = recentPackets[i];
                tft.setCursor(10, y);
                tft.printf("CH%u %02X%02X%02X", p.channel, p.addr[2], p.addr[3], p.addr[4]);

                // Decode type inline
                if (p.len >= 8) {
                    if (p.data[0] < 0x08 && p.data[2] < 0xE8) {
                        tft.print(" KBD");
                    } else if (p.data[0] < 0x08) {
                        tft.print(" MOUSE");
                    } else {
                        tft.print(" DATA");
                    }
                } else {
                    tft.printf(" L%u", p.len);
                }

                y += 12;
            }

            // Show device summary
            if (devices.size() > 0) {
                y += 10;
                tft.setCursor(10, y);
                tft.println("Active devices:");
                y += 12;

                // Sort by hits
                std::vector<NrfSniffedDevice> sorted = devices;
                std::sort(sorted.begin(), sorted.end(), [](const NrfSniffedDevice &a, const NrfSniffedDevice &b) {
                    return a.hits > b.hits;
                });

                for (size_t i = 0; i < sorted.size() && i < 3; i++) {
                    tft.setCursor(10, y);
                    tft.printf("%s (%lu)", addrToString(sorted[i].addr).c_str(), sorted[i].hits);
                    y += 12;
                }
            }

            lastDraw = millis();
        }
    }

    if (logFile) {
        logFile.println("---");
        logFile.printf("Ended: %lu ms\n", millis());
        logFile.printf("Total packets captured: %lu\n", totalPackets);
        logFile.close();
        Serial.println("[NRF] Log file closed");
    }

    NRFradio.stopListening();
    NRFradio.powerDown();

    drawMainBorder();
    tft.setCursor(10, 20);
    tft.setTextSize(FM);
    tft.println("Packet Analysis Complete");
    tft.setTextSize(FP);
    tft.printf("Total packets: %lu\n", totalPackets);
    tft.printf("Unique devices: %u\n", devices.size());
    if (canLog) {
        tft.println("Log saved to SD card");
    }
    tft.println("\nEsc to exit");
    while (!check(EscPress)) delay(50);
}
