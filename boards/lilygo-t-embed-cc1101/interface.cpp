#include "core/powerSave.h"
#include <bq27220.h>
#include <globals.h>
#include <interface.h>

#include <RotaryEncoder.h>
// extern RotaryEncoder encoder;
extern RotaryEncoder *encoder;
IRAM_ATTR void checkPosition();

// Forward declaration for input helper
static void forceClearAllInputs();

// Battery libs
#if defined(T_EMBED_1101)
// Power handler for battery detection
#include <Wire.h>
// Charger chip
#define XPOWERS_CHIP_BQ25896
#include <XPowersLib.h>
#include <esp32-hal-dac.h>
XPowersPPM PPM;
#elif defined(T_EMBED)
#include <driver/adc.h>
#include <esp_adc_cal.h>
#include <soc/adc_channel.h>
#include <soc/soc_caps.h>
#endif

#ifdef USE_BQ27220_VIA_I2C
#define BATTERY_DESIGN_CAPACITY 1300
#include <bq27220.h>
BQ27220 bq;
#endif

#include "core/i2c_finder.h"
#include "modules/rf/rf_utils.h"
#include <Adafruit_PN532.h>

/***************************************************************************************
** Function name: _setup_gpio()
** Description:   initial setup for the device
***************************************************************************************/
void _setup_gpio() {
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);
    pinMode(SEL_BTN, INPUT);
#ifdef T_EMBED_1101
    // T-Embed CC1101 has a antenna circuit optimized to each frequency band, controlled by SW0 and SW1
    // Set antenna frequency settings
    pinMode(CC1101_SW1_PIN, OUTPUT);
    pinMode(CC1101_SW0_PIN, OUTPUT);

    // Chip Select CC1101, SD and TFT to HIGH State to fix SD initialization
    pinMode(CC1101_SS_PIN, OUTPUT);
    digitalWrite(CC1101_SS_PIN, HIGH);
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);
    pinMode(SDCARD_CS, OUTPUT);
    digitalWrite(SDCARD_CS, HIGH);

    // Power chip pin
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH); // Power on CC1101 and LED
    bool pmu_ret = false;
    Wire.begin(GROVE_SDA, GROVE_SCL);
    pmu_ret = PPM.init(Wire, GROVE_SDA, GROVE_SCL, BQ25896_SLAVE_ADDRESS);
    if (pmu_ret) {
        PPM.setSysPowerDownVoltage(3300);
        PPM.setInputCurrentLimit(3250);
        Serial.printf("getInputCurrentLimit: %d mA\n", PPM.getInputCurrentLimit());
        PPM.disableCurrentLimitPin();
        PPM.setChargeTargetVoltage(4208);
        PPM.setPrechargeCurr(64);
        PPM.setChargerConstantCurr(832);
        PPM.getChargerConstantCurr();
        Serial.printf("getChargerConstantCurr: %d mA\n", PPM.getChargerConstantCurr());
        PPM.enableMeasure(PowersBQ25896::CONTINUOUS);
        PPM.disableOTG();
        PPM.enableCharge();
    }
    if (bq.getDesignCap() != BATTERY_DESIGN_CAPACITY) { bq.setDesignCap(BATTERY_DESIGN_CAPACITY); }
    // Start with default IR, RF and RFID Configs, replace old
    // TODO: These fields were removed from BruceConfig - may need restoration if needed
    // bruceConfig.rfModule = CC1101_SPI_MODULE;
    // bruceConfig.rfidModule = PN532_I2C_MODULE;
    // bruceConfig.irRx = 1;
#else
    pinMode(BAT_PIN, INPUT); // Battery value
    Wire.begin(GROVE_SDA, GROVE_SCL);
    Wire.beginTransmission(0x40);
    if (Wire.endTransmission() == 0) {
        Serial.println("ES7210 Online, No CC1101 version");
        Wire.end();
    } else {
        Serial.println("Probably CC1101 exists");
        bruceConfigPins.CC1101_bus.cs = GPIO_NUM_17;
        bruceConfigPins.CC1101_bus.io0 = GPIO_NUM_18;
        bruceConfig.rfModule = CC1101_SPI_MODULE;

        //* If it does not exist, then the CC1101 shield may exist, so there is no need for Wire to exist.
        Wire.endTransmission();
        Wire.end();
    }
    bruceConfig.rfidModule = PN532_SPI_MODULE;

#endif

#ifdef T_EMBED_1101
    pinMode(BK_BTN, INPUT);
#endif
    pinMode(ENCODER_KEY, INPUT);
    // use TWO03 mode when PIN_IN1, PIN_IN2 signals are both LOW or HIGH in latch position.
    encoder = new RotaryEncoder(ENCODER_INA, ENCODER_INB, RotaryEncoder::LatchMode::TWO03);

    // register interrupt routine
    attachInterrupt(digitalPinToInterrupt(ENCODER_INA), checkPosition, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENCODER_INB), checkPosition, CHANGE);
}

/***************************************************************************************
** Function name: getBattery()
** Description:   Delivers the battery value from 1-100
***************************************************************************************/
int getBattery() {
    int percent = 0;
#if defined(USE_BQ27220_VIA_I2C)
    percent = bq.getChargePcnt();
#elif defined(T_EMBED)
    uint8_t _batAdcCh = ADC1_GPIO4_CHANNEL;
    uint8_t _batAdcUnit = 1;
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten((adc1_channel_t)_batAdcCh, ADC_ATTEN_DB_12);
    static esp_adc_cal_characteristics_t *adc_chars = nullptr;
    static constexpr int BASE_VOLATAGE = 3600;
    adc_chars = (esp_adc_cal_characteristics_t *)calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_characterize(
        (adc_unit_t)_batAdcUnit, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, BASE_VOLATAGE, adc_chars
    );
    int raw;
    raw = adc1_get_raw((adc1_channel_t)_batAdcCh);
    uint32_t volt = esp_adc_cal_raw_to_voltage(raw, adc_chars);

    float mv = volt * 2;
    percent = (mv - 3300) * 100 / (float)(4150 - 3350);
#endif

    return (percent < 0) ? 0 : (percent >= 100) ? 100 : percent;
}
/*********************************************************************
**  Function: setBrightness
**  set brightness value
**********************************************************************/
void _setBrightness(uint8_t brightval) {
    if (brightval == 0) {
        analogWrite(TFT_BL, brightval);
    } else if (brightval > 99) {
        analogWrite(TFT_BL, 254);
    } else {
        int bl = MINBRIGHT + round(((255 - MINBRIGHT) * brightval / 100));
        analogWrite(TFT_BL, bl);
    }
}

// RotaryEncoder encoder(ENCODER_INA, ENCODER_INB, RotaryEncoder::LatchMode::TWO03);
RotaryEncoder *encoder = nullptr;
IRAM_ATTR void checkPosition() {
    encoder->tick(); // just call tick() to check the state.
}

/*********************************************************************
** Function: InputHandler
** Handles the variables PrevPress, NextPress, SelPress, AnyKeyPress and EscPress
**********************************************************************/
void InputHandler(void) {
    static unsigned long tm = millis();  // debounce for buttons - increased to 250ms
    static unsigned long tm2 = millis(); // delay between Select and encoder (avoid missclick)
    static bool selHeld = false;
    static unsigned long selPressedAt = 0;
#ifdef T_EMBED_1101
    static bool escHeld = false;
    static unsigned long escPressedAt = 0;
#endif
    static int _last_dir = 0;
    static int _noise_counter = 0;
    static unsigned long _last_noise_reset = 0;
    const unsigned long now = millis();

    // Track raw press/release timing to log hold durations.
    const bool selRawPressed = digitalRead(SEL_BTN) == BTN_ACT;
#ifdef T_EMBED_1101
    const bool escRawPressed = digitalRead(BK_BTN) == BTN_ACT;
#endif

    // Anti-noise: reset counter every 2 seconds
    if (now - _last_noise_reset > 2000) {
        _noise_counter = 0;
        _last_noise_reset = now;
    }

    if (selRawPressed && !selHeld) {
        selHeld = true;
        selPressedAt = now;
    } else if (!selRawPressed && selHeld) {
        const unsigned long heldMs = now - selPressedAt;
        Serial.printf("Select button held for %lums\n", static_cast<unsigned long>(heldMs));
        selHeld = false;
        // Increased debounce time from 200ms to 300ms and require minimum 50ms hold
        if (heldMs >= 50 && now - tm2 > 300) {
            _last_dir = 0;
            SelPress = true;
            tm = now;
        } else if (heldMs < 50) {
            // Potential noise/bounce detected
            _noise_counter++;
            Serial.printf("Potential bounce detected (hold=%lums, count=%d)\n", heldMs, _noise_counter);
        }
    }

#ifdef T_EMBED_1101
    if (escRawPressed && !escHeld) {
        escHeld = true;
        escPressedAt = now;
    } else if (!escRawPressed && escHeld) {
        const unsigned long heldMs = now - escPressedAt;
        Serial.printf("Back button held for %lums\n", heldMs);
        escHeld = false;
        // Require minimum 50ms hold for back button too
        if (heldMs < 50) {
            _noise_counter++;
            Serial.printf("Back button bounce detected (hold=%lums, count=%d)\n", heldMs, _noise_counter);
            return; // Ignore this press
        }
    }
#endif

    // If too many bounces detected (>10 in 2 seconds), ignore all inputs for 500ms
    if (_noise_counter > 10) {
        Serial.println("Too many bounces detected, ignoring inputs for 500ms");
        delay(500);
        _noise_counter = 0;
        _last_noise_reset = now;
        forceClearAllInputs();
        return;
    }

    bool sel = !BTN_ACT;
    bool esc = !BTN_ACT;
    _last_dir = (int)encoder->getDirection();

    // Increased debounce from 200ms to 250ms
    if (now - tm > 250 || LongPress) {
        sel = selRawPressed ? BTN_ACT : !BTN_ACT;
#ifdef T_EMBED_1101
        esc = escRawPressed ? BTN_ACT : !BTN_ACT;
#endif
    }
    if (_last_dir != 0 || sel == BTN_ACT || esc == BTN_ACT) {
        if (!wakeUpScreen()) AnyKeyPress = true;
        else return;
    }
    if (_last_dir > 0) {
        _last_dir = 0;
        PrevPress = true;
#ifdef HAS_ENCODER_LED
        EncoderLedChange = -1;
#endif
        tm2 = millis();
    }
    if (_last_dir < 0) {
        _last_dir = 0;
        NextPress = true;
#ifdef HAS_ENCODER_LED
        EncoderLedChange = 1;
#endif
        tm2 = now;
    }

    if (esc == BTN_ACT) {
        AnyKeyPress = true;
        EscPress = true;
        Serial.println("EscPressed");
        tm = now;
    }
}

// Helper function to clear all inputs - used after noisy events
static void forceClearAllInputs() {
    EscPress = false;
    SelPress = false;
    PrevPress = false;
    NextPress = false;
    UpPress = false;
    DownPress = false;
    AnyKeyPress = false;
    SerialCmdPress = false;
}

void powerOff() {
#ifdef T_EMBED_1101
    PPM.shutdown();
#endif
}

void powerDownNFC() {
    Adafruit_PN532 nfc = Adafruit_PN532(17, 45);
    bool i2c_check = check_i2c_address(PN532_I2C_ADDRESS);
    nfc.setInterface(GROVE_SDA, GROVE_SCL);
    nfc.begin();
    uint32_t versiondata = nfc.getFirmwareVersion();
    if (i2c_check || versiondata) {
        nfc.powerDown();
    } else {
        Serial.println("Can't powerDown PN532");
    }
}

void powerDownCC1101() {
    // Use default frequency 433.92 MHz if rfFreq field not available
    if (!initRfModule("rx", 433.92)) { Serial.println("Can't init CC1101"); }

    ELECHOUSE_cc1101.goSleep();
}

void checkReboot() {
#ifdef T_EMBED_1101
    // Back button long-hold no longer forces deep sleep; emergency reboot logic handles long holds.
#endif
}
/***************************************************************************************
** Function name: isCharging()
** Description:   Determines if the device is charging
***************************************************************************************/
#ifdef USE_BQ27220_VIA_I2C
bool isCharging() {
    return bq.getIsCharging(); // Return the charging status from BQ27220
}
#else
bool isCharging() { return false; }
#endif

/***************************************************************************************
** Function name: getBatteryVoltage()
** Description:   Returns the current battery voltage (in Volts) if supported
***************************************************************************************/
#ifdef USE_BQ27220_VIA_I2C
float getBatteryVoltage() {
    // Return voltage from BQ27220 in Volts (getVolt returns millivolts)
    return bq.getVolt(VOLT) / 1000.0f;
}
#else
float getBatteryVoltage() {
    // TODO: Implement voltage reading for non-BQ27220 devices
    return 0.0f;
}
#endif
