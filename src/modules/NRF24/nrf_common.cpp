#include "nrf_common.h"
#include "../../core/mykeyboard.h"

RF24 NRFradio(NRF24_CE_PIN, NRF24_SS_PIN);
SPIClass *NRFSPI;

void nrf_info() {
    tft.fillScreen(bruceConfig.bgColor);
    tft.setTextSize(FM);
    tft.setTextColor(TFT_RED, bruceConfig.bgColor);
    tft.drawCentreString("_Disclaimer_", tftWidth / 2, 10, 1);
    tft.setTextColor(TFT_WHITE, bruceConfig.bgColor);
    tft.setTextSize(FP);
    tft.setCursor(15, 33);
    padprintln("These functions were made to be used in a controlled environment for STUDY only.");
    padprintln("");
    padprintln("DO NOT use these functions to harm people or companies, you can go to jail!");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    padprintln("");
    padprintln(
        "This device is VERY sensible to noise, so long wires or passing near VCC line can make "
        "things go wrong."
    );
    delay(1000);
    while (!check(AnyKeyPress));
}

bool nrf_start(NRF24_MODE mode) {
    // Note: mode parameter added for compatibility with upstream
    // Currently only SPI mode is implemented
    if (mode != NRF_MODE_SPI) {
        Serial.printf("[NRF24] Warning: Only SPI mode supported, requested mode=%d\n", mode);
    }

    // Validate pins before use
    if (bruceConfigPins.NRF24_bus.cs == GPIO_NUM_NC || bruceConfigPins.NRF24_bus.io0 == GPIO_NUM_NC) {
        Serial.println("[NRF24] Error: CS or CE pin not configured");
        return false;
    }

    Serial.printf("[NRF24] Initializing with CE=%d, CS=%d, mode=%d\n",
                  bruceConfigPins.NRF24_bus.io0, bruceConfigPins.NRF24_bus.cs, mode);

    pinMode(bruceConfigPins.NRF24_bus.cs, OUTPUT);
    digitalWrite(bruceConfigPins.NRF24_bus.cs, HIGH);
    pinMode(bruceConfigPins.NRF24_bus.io0, OUTPUT);
    digitalWrite(bruceConfigPins.NRF24_bus.io0, LOW);

    if (bruceConfigPins.NRF24_bus.mosi == (gpio_num_t)TFT_MOSI &&
        bruceConfigPins.NRF24_bus.mosi != GPIO_NUM_NC) { // (T_EMBED), CORE2 and others
#if TFT_MOSI > 0 // condition for Headless and 8bit displays (no SPI bus)
        NRFSPI = &tft.getSPIinstance();
        Serial.println("[NRF24] Using TFT SPI bus");
#else
        NRFSPI = &SPI;
        Serial.println("[NRF24] Using default SPI bus");
#endif

    } else if (bruceConfigPins.NRF24_bus.mosi == bruceConfigPins.SDCARD_bus.mosi) {
        // NRF24 shares SPI with SDCard (Cardputer and CYDs)
        NRFSPI = &sdcardSPI;
        Serial.println("[NRF24] Using SD Card SPI bus");
    } else if (bruceConfigPins.NRF24_bus.mosi == bruceConfigPins.CC1101_bus.mosi &&
               bruceConfigPins.NRF24_bus.mosi != bruceConfigPins.SDCARD_bus.mosi) {
        // Smoochie board shares CC1101 and NRF24 SPI bus with different CS pins
        NRFSPI = &CC_NRF_SPI;
        Serial.println("[NRF24] Using CC1101/NRF24 shared SPI bus");
    } else {
        NRFSPI = &SPI;
        Serial.println("[NRF24] Using default SPI bus");
    }

    // Initialize SPI bus
    NRFSPI->begin(
        (int8_t)bruceConfigPins.NRF24_bus.sck,
        (int8_t)bruceConfigPins.NRF24_bus.miso,
        (int8_t)bruceConfigPins.NRF24_bus.mosi
    );
    Serial.printf("[NRF24] SPI init: SCK=%d, MISO=%d, MOSI=%d\n",
                  bruceConfigPins.NRF24_bus.sck,
                  bruceConfigPins.NRF24_bus.miso,
                  bruceConfigPins.NRF24_bus.mosi);
    delay(10);

    // Initialize NRF24 radio with CE and CS pins
    bool result = NRFradio.begin(
        NRFSPI,
        rf24_gpio_pin_t(bruceConfigPins.NRF24_bus.io0),  // CE pin
        rf24_gpio_pin_t(bruceConfigPins.NRF24_bus.cs)    // CS pin
    );

    if (result) {
        Serial.println("[NRF24] Radio initialized");
        // Test if radio is responding
        if (!NRFradio.isChipConnected()) {
            Serial.println("[NRF24] WARNING: Chip not responding!");
            return false;
        }
        Serial.println("[NRF24] Chip connected");
    } else {
        Serial.println("[NRF24] ERROR: Init failed");
    }

    return result;
}
