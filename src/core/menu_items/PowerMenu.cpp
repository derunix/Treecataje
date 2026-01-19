#include "PowerMenu.h"
#include "core/batteryLogger.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/utils.h"
#include "interface.h"
#include <Arduino.h>

void PowerMenu::optionsMenu() {
    options.clear();

    options.push_back({"Reboot", []() {
        drawMainBorder();
        tft.setCursor(10, 20);
        tft.setTextSize(FM);
        tft.println("Reboot Device");
        tft.setTextSize(FP);
        tft.println("");
        tft.println("Are you sure?");
        tft.println("");
        tft.println("Select to confirm");
        tft.println("ESC to cancel");

        while (!check(AnyKeyPress) && !check(EscPress) && !check(SelPress)) {
            delay(50);
        }

        if (check(SelPress)) {
            tft.fillScreen(bruceConfig.bgColor);
            tft.setCursor(10, 20);
            tft.setTextSize(FM);
            tft.println("Rebooting...");
            delay(1000);
            ESP.restart();
        }
    }});

    options.push_back({"Power Off", []() {
        drawMainBorder();
        tft.setCursor(10, 20);
        tft.setTextSize(FM);
        tft.println("Powering Off...");
        tft.setTextSize(FP);
        tft.println("");
        tft.println("Goodbye!");
        delay(1000);
        powerOff();
    }});

    options.push_back({"Deep Sleep", []() {
        drawMainBorder();
        tft.setCursor(10, 20);
        tft.setTextSize(FM);
        tft.println("Entering Deep Sleep");
        tft.setTextSize(FP);
        tft.println("");
        tft.println("Press button to wake up");
        delay(1500);
        goToDeepSleep();
    }});

    options.push_back({"Battery Graph", []() {
        BatteryLogger::showLogAsGraph();
    }});

    options.push_back({"Log Settings", []() {
        options.clear();

        const char* intervals[] = {"Off", "1 min", "5 min", "15 min", "30 min", "1 hour"};
        int intervalValues[] = {0, 60, 300, 900, 1800, 3600};

        for (int i = 0; i < 6; i++) {
            bool selected = (bruceConfig.batteryLogInterval == intervalValues[i]);
            options.push_back({intervals[i], [i, intervalValues]() {
                bruceConfig.setBatteryLogInterval(intervalValues[i]);
                bruceConfig.saveFile();
                BatteryLogger::updateIntervalFromConfig(false);
            }, selected});
        }

        options.push_back({"Clear Log", []() {
            drawMainBorder();
            tft.setCursor(10, 20);
            tft.setTextSize(FM);
            tft.println("Clear Battery Log");
            tft.setTextSize(FP);
            tft.println("");
            tft.println("Are you sure?");
            tft.println("");
            tft.println("Select to confirm");
            tft.println("ESC to cancel");

            while (!check(AnyKeyPress) && !check(EscPress) && !check(SelPress)) {
                delay(50);
            }

            if (check(SelPress)) {
                BatteryLogger::deleteLogFile();
                tft.fillScreen(bruceConfig.bgColor);
                tft.setCursor(10, 20);
                tft.setTextSize(FM);
                tft.println("Log cleared");
                delay(1000);
            }
        }});

        addOptionToMainMenu();
        loopOptions(options, MENU_TYPE_SUBMENU, "Logging");
    }});

    options.push_back({"Battery Info", []() {
        drawMainBorder();
        tft.setCursor(10, 20);
        tft.setTextSize(FM);
        tft.println("Battery Status");
        tft.setTextSize(FP);
        tft.println("");

        int batteryLevel = getBattery();
        float voltage = getBatteryVoltage();
        bool charging = isCharging();

        tft.printf("Level: %d%%\n", batteryLevel);
        tft.printf("Voltage: %.2fV\n", voltage);
        tft.printf("Status: %s\n", charging ? "Charging" : "On Battery");
        tft.println("");
        tft.println("Press any key to return");

        while (!check(AnyKeyPress) && !check(EscPress) && !check(SelPress)) {
            delay(50);
        }
    }});

    addOptionToMainMenu();

    loopOptions(options, MENU_TYPE_SUBMENU, "Power");
}

void PowerMenu::drawIconImg() {
    drawImg(
        *bruceConfig.themeFS(),
        bruceConfig.getThemeItemImg(bruceConfig.theme.paths.others),
        0,
        imgCenterY,
        true
    );
}

void PowerMenu::drawIcon(float scale) {
    clearIconArea();
    int battW = scale * 60;
    int battH = scale * 30;
    int x = iconCenterX - battW / 2;
    int y = iconCenterY - battH / 2;

    // Battery outline
    tft.drawRoundRect(x, y, battW, battH, battH / 6, bruceConfig.priColor);

    // Battery terminal
    int termW = scale * 4;
    int termH = battH / 3;
    tft.fillRect(x + battW, y + battH / 2 - termH / 2, termW, termH, bruceConfig.priColor);

    // Battery level
    int batteryLevel = getBattery();
    int fillW = (battW - 8) * batteryLevel / 100;
    if (fillW > 0) {
        tft.fillRect(x + 4, y + 4, fillW, battH - 8, bruceConfig.priColor);
    }

    // Lightning bolt if charging
    if (isCharging()) {
        int boltX = iconCenterX;
        int boltY = iconCenterY;
        int boltSize = scale * 15;

        // Draw lightning bolt
        tft.fillTriangle(
            boltX, boltY - boltSize / 2,
            boltX + boltSize / 3, boltY,
            boltX - boltSize / 4, boltY,
            bruceConfig.secColor
        );
        tft.fillTriangle(
            boltX, boltY + boltSize / 2,
            boltX - boltSize / 3, boltY,
            boltX + boltSize / 4, boltY,
            bruceConfig.secColor
        );
    }
}
