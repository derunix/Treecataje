/**
 * @file wardriving.cpp
 * @author IncursioHack - https://github.com/IncursioHack
 * @brief WiFi Wardriving
 * @version 0.2
 * @note Updated: 2024-08-28 by Rennan Cockles (https://github.com/rennancockles)
 */

#include "wardriving.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/wifi/wifi_common.h"
#include "current_year.h"
#include <TinyGPS++.h>

#define MAX_WAIT 5000

Wardriving::Wardriving() { setup(); }

Wardriving::~Wardriving() {
    if (gpsConnected) end();
    ioExpander.turnPinOnOff(IO_EXP_GPS, LOW);
#ifdef USE_BOOST /// ENABLE 5V OUTPUT
    PPM.disableOTG();
#endif
}

void Wardriving::setup() {
    ioExpander.turnPinOnOff(IO_EXP_GPS, HIGH);
#ifdef USE_BOOST /// ENABLE 5V OUTPUT
    PPM.enableOTG();
#endif
    display_banner();
    padprintln("Initializing...");

    begin_wifi();
    if (!begin_gps()) return;

    vTaskDelay(500 / portTICK_PERIOD_MS);
    return loop();
}

void Wardriving::begin_wifi() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
}

bool Wardriving::begin_gps() {
    gps_provider_begin();
    int count = 0;
    padprintln("Waiting for GPS data");
    while (!gps_provider_seen_bytes(false)) {
        if (check(EscPress)) {
            end();
            return false;
        }
        displayTextLine("Waiting GPS: " + String(count) + "s");
        count++;
        gps_provider_tick();
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    return true;
}

void Wardriving::end() {
    wifiDisconnect();

    gps_provider_end();

    returnToMenu = true;
}

void Wardriving::loop() {
    int count = 0;
    returnToMenu = false;
    while (1) {
        display_banner();

        if (check(EscPress) || returnToMenu) return end();

        gps_provider_tick();

        if (gps_provider_fix_updated(true)) {
            fix = gps_provider_get_fix();
            if (fix.fix_valid) {
                padprintln("GPS location updated");
                set_position();
                scan_networks();
            } else {
                padprintln("GPS data invalid");
                dump_gps_data();
            }
        } else {
            if (count > 5 && !gps_provider_seen_bytes(false)) {
                displayError("GPS not Found!");
                return end();
            }
            padprintln("GPS location not updated");
            dump_gps_data();

            if (filename == "" && fix.year >= CURRENT_YEAR && fix.year < CURRENT_YEAR + 5) create_filename();
            count++;
        }

        int tmp = millis();
        while (millis() - tmp < MAX_WAIT && !gps_provider_fix_updated(false)) {
            gps_provider_tick();
            if (check(EscPress) || returnToMenu) return end();
        }
    }
}

void Wardriving::set_position() {
    double lat = fix.lat_deg;
    double lng = fix.lon_deg;

    if (initial_position_set) distance += TinyGPSPlus::distanceBetween(cur_lat, cur_lng, lat, lng);
    else initial_position_set = true;

    cur_lat = lat;
    cur_lng = lng;
}

void Wardriving::display_banner() {
    drawMainBorderWithTitle("Wardriving");
    padprintln("");

    if (wifiNetworkCount > 0) {
        padprintln("File: " + filename.substring(0, filename.length() - 4), 2);
        padprintln("Unique Networks Found: " + String(wifiNetworkCount), 2);
        padprintf(2, "Distance: %.2fkm\n", distance / 1000);
    }

    padprintln("");
}

void Wardriving::dump_gps_data() {
    if (!date_time_updated && fix.year == 0) {
        padprintln("Waiting for valid GPS data");
        return;
    }
    date_time_updated = true;
    padprintf(2, "Date: %02d-%02d-%02d\n", fix.year, fix.month, fix.day);
    padprintf(2, "Time: %02d:%02d:%02d\n", fix.hour, fix.min, fix.sec);
    padprintf(2, "Sat:  %d\n", fix.sats_used);
    padprintf(2, "HDOP: %.2f\n", fix.hdop);
}

String Wardriving::auth_mode_to_string(wifi_auth_mode_t authMode) {
    switch (authMode) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
        case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
        case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
        case WIFI_AUTH_WAPI_PSK: return "WAPI_PSK";
        default: return "UNKNOWN";
    }
}

void Wardriving::scan_networks() {
    wifiConnected = true;

    int network_amount = WiFi.scanNetworks();
    if (network_amount == 0) {
        padprintln("No Wi-Fi networks found", 2);
        return;
    }

    padprintf(2, "Coord: %.6f, %.6f\n", fix.lat_deg, fix.lon_deg);
    padprintln("Networks Found: " + String(network_amount), 2);

    return append_to_file(network_amount);
}

void Wardriving::create_filename() {
    char timestamp[20];
    sprintf(
        timestamp,
        "%02d%02d%02d_%02d%02d%02d",
        fix.year % 100,
        fix.month % 100,
        fix.day % 100,
        fix.hour % 100,
        fix.min % 100,
        fix.sec % 100
    );
    filename = String(timestamp) + "_wardriving.csv";
}

void Wardriving::append_to_file(int network_amount) {
    FS *fs;
    if (!getFsStorage(fs)) {
        padprintln("Storage setup error");
        returnToMenu = true;
        return;
    }

    if (filename == "") create_filename();

    if (!(*fs).exists("/BruceWardriving")) (*fs).mkdir("/BruceWardriving");

    bool is_new_file = false;
    if (!(*fs).exists("/BruceWardriving/" + filename)) is_new_file = true;
    File file = (*fs).open("/BruceWardriving/" + filename, is_new_file ? FILE_WRITE : FILE_APPEND);

    if (!file) {
        padprintln("Failed to open file for writing");
        returnToMenu = true;
        return;
    }

    if (is_new_file) {
        file.println(
            "WigleWifi-1.6,appRelease=v" + String(BRUCE_VERSION) + ",model=M5Stack GPS Unit,release=v" +
            String(BRUCE_VERSION) +
            ",device=ESP32 M5Stack,display=SPI TFT,board=ESP32 M5Stack,brand=Bruce,star=Sol,body=4,subBody=1"
        );
        file.println(
            "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,"
            "AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type"
        );
    }

    for (int i = 0; i < network_amount; i++) {
        String macAddress = WiFi.BSSIDstr(i);

        // Check if MAC was already found in this session
        if (registeredMACs.find(macAddress) == registeredMACs.end()) {
            registeredMACs.insert(macAddress); // Adds MAC to file
            int32_t channel = WiFi.channel(i);

            char buffer[512];
            snprintf(
                buffer,
                sizeof(buffer),
                "%s,\"%s\",[%s],%04d-%02d-%02d %02d:%02d:%02d,%d,%d,%d,%f,%f,%f,%f,,,WIFI\n",
                macAddress.c_str(),
                WiFi.SSID(i).c_str(),
                auth_mode_to_string(WiFi.encryptionType(i)).c_str(),
                fix.year,
                fix.month,
                fix.day,
                fix.hour,
                fix.min,
                fix.sec,
                channel,
                channel != 14 ? 2407 + (channel * 5) : 2484,
                WiFi.RSSI(i),
                fix.lat_deg,
                fix.lon_deg,
                fix.alt_m,
                fix.hdop * 1.0
            );
            file.print(buffer);

            wifiNetworkCount++;
        }
    }

    file.close();
}
