#include "NRF24.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/utils.h"
#include "modules/NRF24/nrf_common.h"
#include "modules/NRF24/nrf_jammer.h"
#include "modules/NRF24/nrf_hijack.h"
#include "modules/NRF24/nrf_sniffer.h"
#include "modules/NRF24/nrf_spectrum.h"

void NRF24Menu::optionsMenu() {
    options.clear();

    // Analysis & Monitoring
    options.push_back({"Spectrum Analyzer", [this]() { spectrumMenu(); }});
    options.push_back({"Interactive Sniffer", nrf_sniffer_interactive});
    options.push_back({"Sniffer (Simple)", nrf_sniffer});
    options.push_back({"Packet Analyzer", nrf_packet_analyzer});

    // Attack Tools
    options.push_back({"Jammer", [this]() { jammerMenu(); }});
    options.push_back({"Hijack Tools", [this]() { hijackMenu(); }});

    // Utility
    options.push_back({"Information", nrf_info});

#if defined(ARDUINO_M5STICK_C_PLUS) || defined(ARDUINO_M5STICK_C_PLUS2)
    options.push_back({"Config Pins", [this]() { configMenu(); }});
#endif

    addOptionToMainMenu();
    loopOptions(options, MENU_TYPE_SUBMENU, "NRF24");
}

void NRF24Menu::spectrumMenu() {
    // Determine which SPI bus to use based on hardware configuration
    SPIClass *spi = &SPI;

    if (bruceConfigPins.NRF24_bus.mosi == bruceConfigPins.SDCARD_bus.mosi &&
        bruceConfigPins.NRF24_bus.mosi != GPIO_NUM_NC) {
        spi = &sdcardSPI;
    }
#if TFT_MOSI > 0
    else if (bruceConfigPins.NRF24_bus.mosi == (gpio_num_t)TFT_MOSI) {
        spi = &tft.getSPIinstance();
    }
#endif

    nrf_spectrum(spi);
}

void NRF24Menu::jammerMenu() {
    options.clear();

    options.push_back({"Adaptive Jammer", nrf_jammer_pro});
    options.push_back({"Multi-Mode Jammer", nrf_jammer});
    options.push_back({"Single Channel", nrf_channel_jammer});
    options.push_back({"Channel Hopper", nrf_channel_hopper});
    options.push_back({"Carrier Wave", [this]() { carrierJammerMenu(); }});

    addOptionToMainMenu();
    loopOptions(options, MENU_TYPE_SUBMENU, "Jammer");
}

void NRF24Menu::hijackMenu() {
    options.clear();

    options.push_back({"MouseJack", nrf_mousejack});
    options.push_back({"Random Data Spam", [this]() { randomDataMenu(); }});

    addOptionToMainMenu();
    loopOptions(options, MENU_TYPE_SUBMENU, "Hijack");
}

void NRF24Menu::carrierJammerMenu() {
    drawMainBorder();
    tft.setCursor(10, 20);
    tft.setTextSize(FM);
    tft.println("Carrier Wave Jammer");
    tft.setTextSize(FP);
    tft.println("");
    tft.println("Transmits continuous");
    tft.println("carrier wave for");
    tft.println("maximum disruption");
    tft.println("");
    tft.println("Select channel (1-125):");
    tft.println("");

    String channelStr = keyboard("", 3, "Channel");
    if (channelStr == "\x1B" || channelStr.length() == 0) return;

    int channel = channelStr.toInt();
    if (channel < 1 || channel > 125) {
        displayError("Invalid channel");
        delay(2000);
        return;
    }

    // Start carrier jammer
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
    tft.println("Carrier Jamming");
    tft.setTextSize(FP);
    tft.println("");
    tft.printf("Channel: %d\n", channel);
    tft.printf("Frequency: %d MHz\n", 2400 + channel);
    tft.println("");
    tft.println("Status: ACTIVE");
    tft.println("");
    tft.println("ESC to stop");

    NRFradio.startConstCarrier(RF24_PA_MAX, channel);

    while (!check(EscPress)) {
        delay(100);
    }

    NRFradio.stopConstCarrier();
}

void NRF24Menu::randomDataMenu() {
    drawMainBorder();
    tft.setCursor(10, 20);
    tft.setTextSize(FM);
    tft.println("Random Data Spam");
    tft.setTextSize(FP);
    tft.println("");
    tft.println("Select channel (1-125):");

    String channelStr = keyboard("", 3, "Channel");
    if (channelStr == "\x1B" || channelStr.length() == 0) return;

    int channel = channelStr.toInt();
    if (channel < 1 || channel > 125) {
        displayError("Invalid channel");
        delay(2000);
        return;
    }

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
    NRFradio.setChannel(channel);
    NRFradio.stopListening();
    NRFradio.openWritingPipe(0xDEADBEEFAALL);

    drawMainBorder();
    tft.setCursor(10, 20);
    tft.setTextSize(FM);
    tft.println("Random Data Spam");
    tft.setTextSize(FP);
    tft.println("");
    tft.printf("Channel: %d\n", channel);
    tft.printf("Frequency: %d MHz\n", 2400 + channel);
    tft.println("");

    uint32_t packetCount = 0;
    uint32_t lastDisplay = millis();

    while (!check(EscPress)) {
        uint8_t payload[32];
        for (int i = 0; i < 32; i++) {
            payload[i] = random(0, 256);
        }

        NRFradio.write(payload, 32);
        packetCount++;

        if (millis() - lastDisplay > 500) {
            tft.fillRect(10, 80, tftWidth - 20, 30, bruceConfig.bgColor);
            tft.setCursor(10, 80);
            tft.printf("Packets: %u\n", packetCount);
            tft.println("ESC to stop");
            lastDisplay = millis();
        }

        delay(1);
    }
}

void NRF24Menu::configMenu() {
    uint8_t opt = 0;
    options = {
        {"NRF24 (legacy)",     [&]() { opt = 1; }         },
        {"NRF24 (shared SPI)", [&]() { opt = 2; }         },
        {"Back",               [this]() { optionsMenu(); }},
    };

    loopOptions(options, MENU_TYPE_SUBMENU, "RF Config");
    if (opt == 1) {
        bruceConfigPins.setNrf24Pins(
            {(gpio_num_t)NRF24_SCK_PIN,
             (gpio_num_t)NRF24_MISO_PIN,
             (gpio_num_t)NRF24_MOSI_PIN,
             (gpio_num_t)NRF24_SS_PIN,
             (gpio_num_t)NRF24_CE_PIN,
             GPIO_NUM_NC}
        );
    }
#if CONFIG_SOC_GPIO_OUT_RANGE_MAX > 30
    if (opt == 2) {
        bruceConfigPins.setNrf24Pins(
            {(gpio_num_t)SDCARD_SCK,
             (gpio_num_t)SDCARD_MISO,
             (gpio_num_t)SDCARD_MOSI,
             GPIO_NUM_33,
             GPIO_NUM_32,
             GPIO_NUM_NC}
        );
    }
#endif
}
void NRF24Menu::drawIconImg() {
    drawImg(
        *bruceConfig.themeFS(), bruceConfig.getThemeItemImg(bruceConfig.theme.paths.nrf), 0, imgCenterY, true
    );
}
void NRF24Menu::drawIcon(float scale) {
    clearIconArea();
    int iconW = scale * 80;
    int iconH = scale * 60;

    if (iconW % 2 != 0) iconW++;
    if (iconH % 2 != 0) iconH++;

    int caseW = 3 * iconW / 4;
    int caseH = 2 * iconH / 3;
    int caseX = iconCenterX - iconW / 2;
    int caseY = iconCenterY - iconH / 6;

    int antW = iconW / 8;
    int connR = iconH / 20;

    // Case
    tft.drawRect(caseX, caseY, caseW, caseH, bruceConfig.priColor);

    // Antenna
    tft.fillRect(caseX + caseW, caseY + caseH / 2 - antW / 2, antW, antW, bruceConfig.priColor);
    tft.fillRoundRect(
        caseX + caseW + antW,
        caseY + caseH - iconH,
        antW,
        iconH - caseH / 2 + antW / 2,
        antW / 2,
        bruceConfig.priColor
    );

    // Connectors
    tft.fillCircle(caseX + caseW / 6, caseY + 1 * caseH / 5, connR, bruceConfig.priColor);
    tft.fillCircle(caseX + caseW / 6, caseY + 2 * caseH / 5, connR, bruceConfig.priColor);
    tft.fillCircle(caseX + caseW / 6, caseY + 3 * caseH / 5, connR, bruceConfig.priColor);
    tft.fillCircle(caseX + caseW / 6, caseY + 4 * caseH / 5, connR, bruceConfig.priColor);

    tft.fillCircle(caseX + caseW / 3, caseY + 1 * caseH / 5, connR, bruceConfig.priColor);
    tft.fillCircle(caseX + caseW / 3, caseY + 2 * caseH / 5, connR, bruceConfig.priColor);
    tft.fillCircle(caseX + caseW / 3, caseY + 3 * caseH / 5, connR, bruceConfig.priColor);
    tft.fillCircle(caseX + caseW / 3, caseY + 4 * caseH / 5, connR, bruceConfig.priColor);

    // Chip
    tft.fillRect(
        caseX + caseW - 2 * antW - connR, caseY + caseH / 2 - antW / 2, antW, antW, bruceConfig.priColor
    );
}
