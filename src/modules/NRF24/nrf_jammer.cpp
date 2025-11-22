#include "nrf_jammer.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "nrf_common.h"
#include <globals.h>
#include <algorithm>
#include <esp_system.h>
#include <cstdlib>
#include <vector>

static String addrToString(const uint8_t a[5]) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X", a[0], a[1], a[2], a[3], a[4]);
    return String(buf);
}

/* **************************************************************************************
 ** name : nrf_jammer
 ** details : Starts 2.4GHz jammer using NRF24
 ************************************************************************************** */
void nrf_jammer() {
    byte Test_channels[] = {50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 2,  4,  6,  8,
                            10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48};

    // Channel groups
    byte wifi_channels[] = {
        2,
        7,
        12,
        17,
        22,
        27,
        32,
        37, // WiFi 2412–2442 MHz
        42,
        47,
        52,
        57,
        62,
        67,
        72,
        77 // WiFi 2447–2487 MHz
    };
    byte ble_channels[] = {2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                           22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41};

    byte ble_adv[] = {1, 2, 3, 25, 26, 27, 79, 80, 81};

    byte bluetooth_channels[] = {2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
                                 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
                                 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
                                 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65,
                                 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80};
    byte usb_channels[] = {40, 50, 60};
    byte video_channels[] = {70, 75, 80};
    byte rc_channels[] = {1, 3, 5, 7};
    byte full_channels[] = {1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,
                            17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,
                            33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,
                            49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,
                            65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,
                            81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,
                            97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112,
                            113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124};

    struct jamMode {
        const char *name;
        byte *channels;
        size_t count;
    };
    jamMode modes[] = {
        {"Test        ", Test_channels,      sizeof(Test_channels) / sizeof(Test_channels[0])          },
        {"WiFi        ", wifi_channels,      sizeof(wifi_channels) / sizeof(wifi_channels[0])          },
        {"BLEch ",       ble_channels,       sizeof(ble_channels) / sizeof(ble_channels[0])            },
        {"BLE Adv ",     ble_adv,            sizeof(ble_adv) / sizeof(ble_adv[0])                      },
        {"Bluetooth   ", bluetooth_channels, sizeof(bluetooth_channels) / sizeof(bluetooth_channels[0])},
        {"USB         ", usb_channels,       sizeof(usb_channels) / sizeof(usb_channels[0])            },
        {"Video Stream", video_channels,     sizeof(video_channels) / sizeof(video_channels[0])        },
        {"RC          ", rc_channels,        sizeof(rc_channels) / sizeof(rc_channels[0])              },
        {"Full        ", full_channels,      sizeof(full_channels) / sizeof(full_channels[0])          }
    };
    const size_t modesCount = sizeof(modes) / sizeof(modes[0]);

    if (nrf_start()) {
        Serial.println("NRF24 turned On");

        int modeIndex = 0;
        int hopIndex = 0;
        bool redraw = true;

        NRFradio.setPALevel(RF24_PA_MAX);
        NRFradio.setAddressWidth(5);
        NRFradio.setPayloadSize(2);
        if (!NRFradio.setDataRate(RF24_2MBPS)) Serial.println("Fail setting data Rate");

        auto startCarrierOn = [&](uint8_t channel) {
            NRFradio.startConstCarrier(RF24_PA_MAX, channel);
            delayMicroseconds(250); // allow PLL to lock before hopping again
        };
        startCarrierOn(modes[modeIndex].channels[hopIndex]);
        hopIndex++;
        if (hopIndex >= (int)modes[modeIndex].count) hopIndex = 0;

        drawMainBorder();

        while (!check(SelPress)) {
            if (redraw) {
                tft.setCursor(10, 35);
                tft.setTextSize(FM);
                tft.println("NRF X Jammer");
                tft.setCursor(10, tft.getCursorY() + 25);
                tft.println("STATUS : ACTIVE");
                tft.setCursor(10, 100);
                tft.fillRect(10, 100, tftWidth - 20, FM * LH, bruceConfig.bgColor);
                tft.print("MODE : " + String(modes[modeIndex].name));
                tft.drawRoundRect(5, 5, tftWidth - 10, tftHeight - 10, 5, bruceConfig.priColor);
                Serial.println(modes[modeIndex].name);
                redraw = false;
                delay(200);
            }

            // Change mode
            if (check(NextPress)) {
                modeIndex++;
                if (modeIndex >= (int)modesCount) modeIndex = 0;
                hopIndex = 0;
                redraw = true;
            }
            if (check(PrevPress)) {
                modeIndex--;
                if (modeIndex < 0) modeIndex = (int)modesCount - 1;
                hopIndex = 0;
                redraw = true;
            }

            // Hop through channels with a fresh carrier start on each hop
            startCarrierOn(modes[modeIndex].channels[hopIndex]);
            hopIndex++;
            if (hopIndex >= (int)modes[modeIndex].count) hopIndex = 0;
        }

        NRFradio.stopConstCarrier();

    } else {
        Serial.println("Fail Starting radio");
        displayError("NRF24 not found");
        delay(500);
    }
}

/* **************************************************************************************
 ** name : nrf_channel_jammer
 ** details : Steps manually through each channel 1–125
 ************************************************************************************** */
void nrf_channel_jammer() {
    if (nrf_start()) {
        Serial.println("NRF24 turned On");

        int channel = 50; /// we start at 50 as on lower channel the cw wont work correctly
        bool redraw = true;

        NRFradio.setPALevel(RF24_PA_MAX);
        NRFradio.setAddressWidth(3);
        NRFradio.setPayloadSize(2);
        if (!NRFradio.setDataRate(RF24_2MBPS)) Serial.println("Fail setting data Rate");

        auto startCarrierOn = [&](uint8_t ch) {
            NRFradio.startConstCarrier(RF24_PA_MAX, ch);
            delayMicroseconds(250);
        };
        startCarrierOn(channel);

        drawMainBorder();

        while (!check(SelPress)) {
            if (redraw) {
                int freq = 2400 + channel; // MHz
                tft.setCursor(10, 35);
                tft.setTextSize(FM);
                tft.println("NRF Channel Jammer");
                tft.setCursor(10, tft.getCursorY() + 25);
                tft.println("STATUS : ACTIVE");
                tft.fillRect(10, 100, tftWidth - 20, FM * LH, bruceConfig.bgColor);
                tft.setCursor(10, 100);
                tft.print("MODE : CH " + String(channel));
                tft.setCursor(10, 116);
                tft.fillRect(10, 116, tftWidth - 20, FM * LH, bruceConfig.bgColor);
                tft.printf("Freq : %d MHz", freq);
                Serial.println("CH " + String(channel) + " (" + String(freq) + " MHz)");
                tft.drawRoundRect(5, 5, tftWidth - 10, tftHeight - 10, 5, bruceConfig.priColor);
                redraw = false;
                delay(200);
            }

            // Next/Prev channel
            if (check(NextPress)) {

                channel++;
                if (channel > 125) channel = 1;
                startCarrierOn(channel);
                redraw = true;
            }
            if (check(PrevPress)) {

                channel--;
                if (channel < 1) channel = 125;
                startCarrierOn(channel);
                redraw = true;
            }
        }

        NRFradio.stopConstCarrier();

    } else {
        Serial.println("Fail Starting radio");
        displayError("NRF24 not found");
        delay(500);
    }
}

void nrf_channel_hopper() {
    if (!nrf_start()) {
        Serial.println("Fail Starting radio");
        displayError("NRF24 not found");
        delay(100);
        return;
    }

    Serial.println("NRF24 turned On");
    NRFradio.setPALevel(RF24_PA_MAX);
    NRFradio.setAddressWidth(3);
    NRFradio.setPayloadSize(2);

    if (!NRFradio.setDataRate(RF24_2MBPS)) Serial.println("Fail setting data Rate");

    int startChannel = 0;
    int stopChannel = 80;
    int stepSize = 2;

    auto startCarrierOn = [&](uint8_t ch) {
        NRFradio.startConstCarrier(RF24_PA_MAX, ch);
        delayMicroseconds(250);
    };

    int menuIndex = 0;
    bool redraw = true;
    bool editMode = false;

    bool runJammer = false;
    bool hopmenu = true;

    while (hopmenu) {

        if (redraw) {
            drawMainBorder();
            tft.setCursor(10, 35);
            tft.setTextSize(FM);
            tft.println("NRF Hopper Config");

            tft.setCursor(10, 70);
            tft.printf("Start : CH %d", startChannel);
            tft.setCursor(10, 90);
            tft.printf("Stop  : CH %d", stopChannel);
            tft.setCursor(10, 110);
            tft.printf("Step  : %d mhz", stepSize);
            tft.setCursor(10, 130);
            tft.print("Start Jammer");
            tft.setCursor(10, 150);
            tft.print("Exit");

            int yHighlight = 70; // default avoids uninit warnings
            if (menuIndex == 1) yHighlight = 90;
            else if (menuIndex == 2) yHighlight = 110;
            else if (menuIndex == 3) yHighlight = 130;
            else if (menuIndex == 4) yHighlight = 150;

            tft.drawRect(5, yHighlight - 2, tftWidth - 10, 18, bruceConfig.priColor);
            redraw = false;
        }

        if (check(EscPress)) {
            hopmenu = false;
            return;
        }

        if (check(NextPress)) {
            if (editMode) {
                if (menuIndex == 0) startChannel = (startChannel % 125) + 1;
                if (menuIndex == 1) stopChannel = (stopChannel % 125) + 1;
                if (menuIndex == 2) stepSize = (stepSize % 10) + 1;
            } else {
                menuIndex = (menuIndex + 1) % 5;
            }
            redraw = true;
            delay(150);
        }

        if (check(PrevPress)) {
            if (editMode) {
                if (menuIndex == 0) startChannel = (startChannel - 2 + 125) % 125 + 1;
                if (menuIndex == 1) stopChannel = (stopChannel - 2 + 125) % 125 + 1;
                if (menuIndex == 2) stepSize = (stepSize - 2 + 10) % 10 + 1;
            } else {
                menuIndex = (menuIndex - 1 + 5) % 5;
            }
            redraw = true;
            delay(150);
        }

        if (check(SelPress)) {
            if (menuIndex == 3 && !editMode) {

                runJammer = true;
                hopmenu = false;
            } else if (menuIndex == 4 && !editMode) {

                hopmenu = false;
                return;
            } else {
                if (menuIndex < 3) editMode = !editMode;
            }
            redraw = true;
            delay(150);
        }
    }

    if (runJammer) {
        int channel = startChannel;
        drawMainBorder();
        tft.setCursor(10, 35);
        tft.setTextSize(FM);
        tft.println("NRF Hopper Jammer");
        tft.setCursor(10, 70);
        tft.printf("Range : %d - %d", startChannel, stopChannel);
        tft.setCursor(10, 90);
        tft.printf("Step  : %d", stepSize);

        startCarrierOn(channel);
        while (!check(EscPress)) {
            channel += stepSize;
            if (channel > stopChannel) channel = startChannel;
            startCarrierOn(channel);
        }

        NRFradio.stopConstCarrier();
        Serial.println("Jammer Stopped");
    }
}

// New configurable jammer with targeted spam/sweep/noise
void nrf_jammer_pro() {
    enum JamMode { MODE_CARRIER, MODE_NOISE, MODE_SPAM, MODE_TARGET_SPAM, MODE_HYBRID };
    enum JamPreset { PRESET_FULL, PRESET_WIFI, PRESET_BLE_ADV, PRESET_BLE_DATA, PRESET_CUSTOM, PRESET_TARGET };
    const char *modeNames[] = {"Carrier", "Noise", "Rand spam", "Target spam", "Hybrid"};
    const char *presetNames[] = {"Full band", "WiFi grid", "BLE adv", "BLE data", "Custom sweep", "Target CH"};
    const char *rateNames[] = {"1Mbps", "2Mbps"};
    const rf24_pa_dbm_e paLevels[] = {RF24_PA_MIN, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX};
    const char *paNames[] = {"-18", "-12", "-6", "MAX"};

    static const uint8_t wifiGrid[] = {2,  7,  12, 17, 22, 27, 32, 37, 42, 47, 52, 57, 62, 67, 72, 77};
    static const uint8_t bleAdv[] = {1, 2, 3, 25, 26, 27, 79, 80, 81};
    static const uint8_t bleData[] = {2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                                      20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
                                      39, 40, 41};

    static const uint8_t defaultPipe[5] = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7};

    JamMode mode = MODE_CARRIER;
    JamPreset preset = PRESET_FULL;
    int startCh = 1;
    int stopCh = 80;
    int step = 2;
    int dwell = 80; // ms per hop
    int rateIdx = 0; // 0=1M,1=2M
    int paIdx = 3;   // MAX by default
    uint8_t targetAddr[5] = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7};
    uint8_t targetCh = 2;
    bool useTarget = false;
    int cursor = 0;
    bool editing = false;
    bool uiDirty = true;

    auto clampChannel = [](int c) -> uint8_t {
        if (c < 1) return 1;
        if (c > 125) return 125;
        return (uint8_t)c;
    };

    auto applyRadioTx = [&]() {
        NRFradio.stopListening();
        NRFradio.setAutoAck(false);
        NRFradio.setPALevel(paLevels[paIdx]);
        NRFradio.setAddressWidth(5);
        NRFradio.setDataRate(rateIdx == 0 ? RF24_1MBPS : RF24_2MBPS);
        NRFradio.enableDynamicPayloads();
        NRFradio.setPayloadSize(8);
        NRFradio.openWritingPipe(useTarget ? targetAddr : defaultPipe);
    };

    auto drawUi = [&](bool running) {
        drawMainBorder();
        tft.setTextSize(FP);
        tft.setCursor(8, 6);
        tft.println(running ? "Jammer PRO (RUN)" : "NRF Jammer PRO");
        int y = 24;
        const int rowH = 14;
        auto line = [&](int idx, const char *text) {
            tft.setCursor(8, y);
            tft.print(idx == cursor ? ">" : " ");
            tft.print(text);
            y += rowH;
        };
        char buf[64];
        snprintf(buf, sizeof(buf), "Mode: %s", modeNames[mode]);
        line(0, buf);
        snprintf(buf, sizeof(buf), "Preset: %s", presetNames[preset]);
        line(1, buf);
        snprintf(buf, sizeof(buf), "Start CH: %3d", startCh);
        line(2, buf);
        snprintf(buf, sizeof(buf), "Stop  CH: %3d", stopCh);
        line(3, buf);
        snprintf(buf, sizeof(buf), "Step: %2d", step);
        line(4, buf);
        snprintf(buf, sizeof(buf), "Dwell: %3d ms", dwell);
        line(5, buf);
        snprintf(buf, sizeof(buf), "Rate/PA: %s / %s", rateNames[rateIdx], paNames[paIdx]);
        line(6, buf);
        snprintf(buf, sizeof(buf), "Target CH: %3u", targetCh);
        line(7, buf);
        snprintf(buf, sizeof(buf), "Target ID: %s", useTarget ? addrToString(targetAddr).c_str() : "OFF");
        line(8, buf);
        line(9, running ? "Stop (Sel/Esc)" : "Start (Sel)");

        if (!running) {
            tft.setCursor(8, tftHeight - 16);
            tft.print(editing ? "Edit: Next/Prev change | Long=addr" : "Navigate: Next/Prev  Esc=back");
        }
        uiDirty = false;
    };

    auto buildChannels = [&]() {
        std::vector<uint8_t> list;
        switch (preset) {
        case PRESET_FULL:
            for (int c = 1; c <= 125; ++c) list.push_back((uint8_t)c);
            break;
        case PRESET_WIFI:
            list.assign(wifiGrid, wifiGrid + sizeof(wifiGrid));
            break;
        case PRESET_BLE_ADV:
            list.assign(bleAdv, bleAdv + sizeof(bleAdv));
            break;
        case PRESET_BLE_DATA:
            list.assign(bleData, bleData + sizeof(bleData));
            break;
        case PRESET_CUSTOM:
            if (startCh > stopCh) std::swap(startCh, stopCh);
            for (int c = startCh; c <= stopCh; c += std::max(1, step)) list.push_back((uint8_t)c);
            break;
        case PRESET_TARGET:
            list.push_back(targetCh);
            break;
        }
        if (list.empty()) list.push_back(2);
        return list;
    };

    auto jamCarrier = [&](uint8_t ch) {
        NRFradio.setChannel(ch);
        NRFradio.startConstCarrier(paLevels[paIdx], ch);
        unsigned long endAt = millis() + dwell;
        while ((long)(endAt - millis()) > 0 && !check(EscPress) && !check(SelPress)) { delay(2); }
        NRFradio.stopConstCarrier();
    };

    auto jamNoise = [&](uint8_t ch, bool forceTargetPipe, bool randomPipe) {
        NRFradio.setChannel(ch);
        if (randomPipe) {
            uint8_t addr[5];
            for (uint8_t &b : addr) b = (uint8_t)esp_random();
            NRFradio.openWritingPipe(addr);
        } else if (forceTargetPipe && useTarget) {
            NRFradio.openWritingPipe(targetAddr);
        }
        unsigned long endAt = millis() + dwell;
        while ((long)(endAt - millis()) > 0 && !check(EscPress) && !check(SelPress)) {
            uint8_t buf[8];
            for (uint8_t &b : buf) b = (uint8_t)esp_random();
            NRFradio.writeFast(buf, sizeof(buf));
            delay(2);
        }
    };

    if (!nrf_start()) {
        Serial.println("Fail Starting radio");
        displayError("NRF24 not found");
        delay(500);
        return;
    }

    drawUi(false);
    const int fieldCount = 10;
    while (true) {
        if (check(EscPress)) return;
        if (check(LongPress) && cursor == 8) {
            String hex = keyboard(addrToString(targetAddr), 10, "Target addr (hex)");
            if (hex.length() >= 10) {
                for (int i = 0; i < 5; ++i) {
                    char b[3] = {hex[i * 2], hex[i * 2 + 1], 0};
                    targetAddr[i] = (uint8_t)strtol(b, nullptr, 16);
                }
                useTarget = true;
            } else {
                useTarget = false;
            }
            uiDirty = true;
        }

        if (check(SelPress)) {
            if (cursor == 9) break; // start
            editing = !editing;
            uiDirty = true;
        } else if (check(NextPress)) {
            if (editing) {
                switch (cursor) {
                case 0: mode = (JamMode)((mode + 1) % 5); break;
                case 1: preset = (JamPreset)((preset + 1) % 6); break;
                case 2: startCh = clampChannel(startCh + 1); break;
                case 3: stopCh = clampChannel(stopCh + 1); break;
                case 4: step = std::min(20, step + 1); break;
                case 5: dwell = std::min(800, dwell + 10); break;
                case 6: rateIdx = (rateIdx + 1) % 2; paIdx = (paIdx + 1) % 4; break;
                case 7: targetCh = clampChannel(targetCh + 1); break;
                case 8: useTarget = !useTarget; break;
                }
            } else {
                cursor = (cursor + 1) % fieldCount;
            }
            uiDirty = true;
        } else if (check(PrevPress)) {
            if (editing) {
                switch (cursor) {
                case 0: mode = (JamMode)((mode + 5 - 1) % 5); break;
                case 1: preset = (JamPreset)((preset + 6 - 1) % 6); break;
                case 2: startCh = clampChannel(startCh - 1); break;
                case 3: stopCh = clampChannel(stopCh - 1); break;
                case 4: step = std::max(1, step - 1); break;
                case 5: dwell = std::max(10, dwell - 10); break;
                case 6: rateIdx = (rateIdx + 1) % 2; paIdx = (paIdx + 3) % 4; break;
                case 7: targetCh = clampChannel(targetCh - 1); break;
                case 8: useTarget = !useTarget; break;
                }
            } else {
                cursor = (cursor + fieldCount - 1) % fieldCount;
            }
            uiDirty = true;
        }

        if (uiDirty) drawUi(false);
        delay(40);
    }

    applyRadioTx();
    auto channels = buildChannels();
    drawUi(true);
    tft.setCursor(8, tftHeight - 28);
    tft.print("Esc/Sel=stop  Long=addr");

    size_t idx = 0;
    while (!check(EscPress) && !check(SelPress)) {
        const uint8_t ch = channels[idx];
        tft.fillRect(8, tftHeight - 46, tftWidth - 16, 14, bruceConfig.bgColor);
        tft.setCursor(8, tftHeight - 46);
        tft.printf("CH %3u (%u/%u)", ch, (unsigned)(idx + 1), (unsigned)channels.size());

        switch (mode) {
        case MODE_CARRIER: jamCarrier(ch); break;
        case MODE_NOISE: jamNoise(ch, false, false); break;
        case MODE_SPAM: jamNoise(ch, false, true); break;
        case MODE_TARGET_SPAM: jamNoise(ch, true, false); break;
        case MODE_HYBRID:
            jamCarrier(ch);
            jamNoise(ch, useTarget, true);
            break;
        }
        if (check(EscPress) || check(SelPress)) break;
        idx = (idx + 1) % channels.size();
    }

    NRFradio.stopConstCarrier();
}
