#include "core/powerSave.h"
#include <driver/adc.h>
#include <esp_adc_cal.h>
#include <interface.h>
#include <soc/adc_channel.h>
#include <soc/soc_caps.h>

/***************************************************************************************
** Function name: _setup_gpio()
** Location: main.cpp
** Description:   initial setup for the device
***************************************************************************************/
void _setup_gpio() {
    pinMode(UP_BTN, INPUT); // Sets the power btn as an INPUT
    pinMode(SEL_BTN, INPUT);
    pinMode(DW_BTN, INPUT);
    pinMode(4, OUTPUT);    // Keeps the Stick alive after take off the USB cable
    digitalWrite(4, HIGH); // Keeps the Stick alive after take off the USB cable
    gpio_pulldown_dis(GPIO_NUM_36);
    gpio_pullup_dis(GPIO_NUM_36);
    pinMode(32, OUTPUT);
    pinMode(33, OUTPUT);
    digitalWrite(32, LOW);
    digitalWrite(33, HIGH);
}

/***************************************************************************************
** Function name: getBattery()
** location: display.cpp
** Description:   Delivers the battery value from 1-100
***************************************************************************************/
int getBattery() {
    uint8_t percent;
    uint8_t _batAdcCh = ADC1_GPIO38_CHANNEL;
    uint8_t _batAdcUnit = 1;
    static uint32_t lastVolt = 5000;
    static unsigned long lastTime = 0;

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

    return (percent >= 100) ? 100 : percent;
}

/*********************************************************************
** Function: setBrightness
** location: settings.cpp
** set brightness value
**********************************************************************/
void _setBrightness(uint8_t brightval) {
    if (brightval == 0) {
        analogWrite(TFT_BL, brightval);
    } else {
        int bl = MINBRIGHT + round(((255 - MINBRIGHT) * brightval / 100));
        analogWrite(TFT_BL, bl);
    }
}

/*********************************************************************
** Function: InputHandler
** Handles the variables PrevPress, NextPress, SelPress, AnyKeyPress and EscPress
**********************************************************************/
void InputHandler(void) {
    static unsigned long tm = 0;
    static unsigned long up_tm = 0, sel_tm = 0, dw_tm = 0;
    static bool up_held = false, sel_held = false, dw_held = false;
    static unsigned long up_press_at = 0, sel_press_at = 0, dw_press_at = 0;
    static int bounce_counter = 0;
    static unsigned long last_bounce_reset = 0;

    // Increased debounce from 200ms to 250ms for better stability
    if (millis() - tm < 250 && !LongPress) return;

    // Reset bounce counter every 2 seconds
    unsigned long now = millis();
    if (now - last_bounce_reset > 2000) {
        bounce_counter = 0;
        last_bounce_reset = now;
    }

    bool upPressed = (digitalRead(UP_BTN) == LOW);
    bool selPressed = (digitalRead(SEL_BTN) == LOW);
    bool dwPressed = (digitalRead(DW_BTN) == LOW);

    // Debounce each button individually with minimum hold time requirement
    bool upValid = false, selValid = false, dwValid = false;

    // UP button processing
    if (upPressed && !up_held) {
        up_held = true;
        up_press_at = now;
    } else if (!upPressed && up_held) {
        unsigned long held_ms = now - up_press_at;
        up_held = false;
        // Require minimum 50ms hold to be valid press
        if (held_ms >= 50 && now - up_tm > 250) {
            upValid = true;
            up_tm = now;
        } else if (held_ms < 50) {
            bounce_counter++;
            Serial.printf("UP button bounce (held=%lums, count=%d)\n", held_ms, bounce_counter);
        }
    }

    // SEL button processing
    if (selPressed && !sel_held) {
        sel_held = true;
        sel_press_at = now;
    } else if (!selPressed && sel_held) {
        unsigned long held_ms = now - sel_press_at;
        sel_held = false;
        if (held_ms >= 50 && now - sel_tm > 250) {
            selValid = true;
            sel_tm = now;
        } else if (held_ms < 50) {
            bounce_counter++;
            Serial.printf("SEL button bounce (held=%lums, count=%d)\n", held_ms, bounce_counter);
        }
    }

    // DOWN button processing
    if (dwPressed && !dw_held) {
        dw_held = true;
        dw_press_at = now;
    } else if (!dwPressed && dw_held) {
        unsigned long held_ms = now - dw_press_at;
        dw_held = false;
        if (held_ms >= 50 && now - dw_tm > 250) {
            dwValid = true;
            dw_tm = now;
        } else if (held_ms < 50) {
            bounce_counter++;
            Serial.printf("DW button bounce (held=%lums, count=%d)\n", held_ms, bounce_counter);
        }
    }

    // If too many bounces, clear inputs and wait
    if (bounce_counter > 8) {
        Serial.println("Too many button bounces detected, waiting 300ms");
        delay(300);
        bounce_counter = 0;
        last_bounce_reset = millis();
        // Clear all volatile flags
        EscPress = false;
        SelPress = false;
        PrevPress = false;
        NextPress = false;
        UpPress = false;
        DownPress = false;
        AnyKeyPress = false;
        return;
    }

    bool anyPressed = upPressed || selPressed || dwPressed;
    if (anyPressed) tm = millis();
    if (anyPressed && wakeUpScreen()) return;

    AnyKeyPress = anyPressed;
    PrevPress = upValid;
    EscPress = upValid;
    NextPress = dwValid;
    SelPress = selValid;
}

/*********************************************************************
** Function: powerOff
** location: mykeyboard.cpp
** Turns off the device (or try to)
**********************************************************************/
void powerOff() {
    digitalWrite(4, LOW);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)UP_BTN, LOW);
    esp_deep_sleep_start();
}

/*********************************************************************
** Function: checkReboot
** location: mykeyboard.cpp
** Btn logic to tornoff the device (name is odd btw)
**********************************************************************/
void checkReboot() {
    int countDown;
    /* Long press power off */
    if (digitalRead(UP_BTN) == LOW) {
        uint32_t time_count = millis();
        while (digitalRead(UP_BTN) == LOW) {
            // Display poweroff bar only if holding button
            if (millis() - time_count > 500) {
                tft.setCursor(60, 12);
                tft.setTextSize(1);
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
                countDown = (millis() - time_count) / 1000 + 1;
                tft.printf(" PWR OFF IN %d/3\n", countDown);
                vTaskDelay(10 / portTICK_RATE_MS);
            }
        }

        // Clear text after releasing the button
        if (millis() - time_count > 500)
            tft.fillRect(60, 12, 16 * LW, tft.fontHeight(1), bruceConfig.bgColor);
        PrevPress = true;
    }
}

bool isCharging() { return false; }
