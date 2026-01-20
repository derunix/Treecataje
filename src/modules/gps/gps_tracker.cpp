/**
 * @file gps_tracker.cpp
 * @author Rennan Cockles (https://github.com/rennancockles)
 * @brief GPS tracker
 * @version 0.1
 * @date 2024-11-20
 */

#include "gps_tracker.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "current_year.h"

#define MAX_WAIT 5000

GPSTracker::GPSTracker() { setup(); }

GPSTracker::~GPSTracker() {
    add_final_file_data();
    if (gpsConnected) end();
    ioExpander.turnPinOnOff(IO_EXP_GPS, LOW);
#ifdef USE_BOOST
    PPM.disableOTG();
#endif
}

void GPSTracker::setup() {
    ioExpander.turnPinOnOff(IO_EXP_GPS, HIGH);
#ifdef USE_BOOST /// ENABLE 5V OUTPUT
    PPM.enableOTG();
#endif
    display_banner();
    padprintln("Initializing...");

    if (!begin_gps()) return;

    return loop();
}

bool GPSTracker::begin_gps() {
    releasePins();

    if (!gps_provider_begin()) {
        padprintln("Failed to initialize GPS provider");
        return false;
    }

    int count = 0;
    padprintln("Waiting for GPS data");
    while (!gps_provider_seen_bytes()) {
        if (check(EscPress)) {
            end();
            return false;
        }
        displayTextLine("Waiting GPS: " + String(count) + "s");
        count++;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    gpsConnected = true;
    return true;
}

void GPSTracker::end() {
    gps_provider_end();
    restorePins();

    // returnToMenu removed - function returns normally, allowing back navigation
    gpsConnected = false;
}

void GPSTracker::loop() {
    int count = 0;
    returnToMenu = false;
    while (1) {
        display_banner();

        if (check(EscPress) || returnToMenu) return end();

        // Update GPS state
        gps_provider_tick();

        if (gps_provider_seen_bytes()) {
            count = 0;

            if (gps_provider_fix_updated(false)) {
                padprintln("GPS location updated");
                fix = gps_provider_get_fix();
                set_position();
                add_coord();
                gps_provider_fix_updated(true); // Clear the flag
            } else {
                padprintln("GPS location not updated");
                fix = gps_provider_get_fix();
                dump_gps_data();

                if (filename == "" && fix.year >= CURRENT_YEAR && fix.year < CURRENT_YEAR + 5)
                    create_filename();
            }
        } else {
            if (count > 5) {
                displayError("GPS not Found!");
                return end();
            }
            padprintln("No GPS data available");
            count++;
        }

        int tmp = millis();
        while (millis() - tmp < MAX_WAIT && !gps_provider_fix_updated(false)) {
            gps_provider_tick();
            if (check(EscPress) || returnToMenu) return end();
        }
    }
}

void GPSTracker::set_position() {
    double lat = fix.lat_deg;
    double lng = fix.lon_deg;

    if (initial_position_set) {
        // Calculate distance using Haversine formula
        double lat1_rad = cur_lat * DEG_TO_RAD;
        double lat2_rad = lat * DEG_TO_RAD;
        double delta_lat = (lat - cur_lat) * DEG_TO_RAD;
        double delta_lng = (lng - cur_lng) * DEG_TO_RAD;
        double a = sin(delta_lat / 2) * sin(delta_lat / 2) +
                   cos(lat1_rad) * cos(lat2_rad) * sin(delta_lng / 2) * sin(delta_lng / 2);
        double c = 2 * atan2(sqrt(a), sqrt(1 - a));
        distance += 6371000 * c; // Earth radius in meters
    } else {
        initial_position_set = true;
    }

    cur_lat = lat;
    cur_lng = lng;
}

void GPSTracker::display_banner() {
    drawMainBorderWithTitle("GPS Tracker");
    padprintln("");

    if (gpsCoordCount > 0) {
        padprintln("File: " + filename.substring(0, filename.length() - 4), 2);
        padprintln("GPS Coordinates: " + String(gpsCoordCount), 2);
        padprintf(2, "Distance: %.2fkm\n", distance / 1000);
    }

    padprintln("");
}

void GPSTracker::dump_gps_data() {
    if (!date_time_updated && (fix.year == 0 || fix.month == 0)) {
        padprintln("Waiting for valid GPS data");
        return;
    }
    date_time_updated = true;
    padprintf(2, "Date: %04d-%02d-%02d\n", fix.year, fix.month, fix.day);
    padprintf(2, "Time: %02d:%02d:%02d\n", fix.hour, fix.min, fix.sec);
    padprintf(2, "Sat:  %d\n", fix.sats_used);
    padprintf(2, "HDOP: %.2f\n", fix.hdop);
}

void GPSTracker::create_filename() {
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
    filename = String(timestamp) + "_gps_tracker.gpx";
}

void GPSTracker::add_initial_file_data(File file) {
    file.println("<?xml version=\"1.0\" encoding=\"ISO-8859-1\" standalone=\"yes\"?>");
    file.println("<?xml-stylesheet type=\"text/xsl\" href=\"details.xsl\"?>");
    file.println("<gpx");
    file.println("  version=\"1.1\"");
    file.println("  creator=\"Bruce Firmware\"");
    file.println("  xmlns=\"http://www.topografix.com/GPX/1/1\"");
    file.println("  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"");
    file.println(
        "  xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1 http://www.topografix.com/GPX/1/1/gpx.xsd\""
    );
    file.println(">");
    file.println("  <metadata>");
    file.println("    <name>Bruce GPS Tracker</name>");
    file.println("    <desc>GPS Tracker using Bruce Firmware</desc>");
    file.println("    <link href=\"https://bruce.computer\">");
    file.println("      <text>Bruce Website</text>");
    file.println("    </link>");
    file.println("  </metadata>");
    file.println("  <trk>");
    file.println("    <name>Bruce Route</name>");
    file.println("    <desc>GPS route captured by Bruce firmware</desc>");
    file.println("    <trkseg>");
}

void GPSTracker::add_final_file_data() {
    FS *fs;
    if (!getFsStorage(fs)) return;
    if (filename == "" || !(*fs).exists("/BruceGPS/" + filename)) return;

    File file = (*fs).open("/BruceGPS/" + filename, FILE_APPEND);

    if (!file) return;
    file.println("    </trkseg>");
    file.println("  </trk>");
    file.println("</gpx>");

    file.close();
}

void GPSTracker::add_coord() {
    FS *fs;
    if (!getFsStorage(fs)) {
        padprintln("Storage setup error");
        return; // Return to previous menu
    }

    if (filename == "") create_filename();

    if (!(*fs).exists("/BruceGPS")) (*fs).mkdir("/BruceGPS");

    bool is_new_file = false;
    if (!(*fs).exists("/BruceGPS/" + filename)) is_new_file = true;
    File file = (*fs).open("/BruceGPS/" + filename, is_new_file ? FILE_WRITE : FILE_APPEND);

    if (!file) {
        padprintln("Failed to open file for writing");
        return; // Return to previous menu
    }

    if (is_new_file) add_initial_file_data(file);

    file.printf("      <trkpt lat=\"%f\" lon=\"%f\">\n", fix.lat_deg, fix.lon_deg);
    file.println("        <sym>Waypoint</sym>");
    file.printf("        <ele>%f</ele>\n", fix.alt_m);
    file.printf("        <hdop>%f</hdop>\n", fix.hdop);
    file.printf("        <sat>%d</sat>\n", fix.sats_used);
    file.println("      </trkpt>");

    gpsCoordCount++;

    file.close();

    padprintf(2, "Coord: %.6f, %.6f\n", fix.lat_deg, fix.lon_deg);
}

void GPSTracker::releasePins() {
    rxPinReleased = false;
    if (bruceConfigPins.CC1101_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
        bruceConfigPins.NRF24_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
#if !defined(LITE_VERSION)
        bruceConfigPins.W5500_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
        bruceConfigPins.LoRa_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
#endif
        bruceConfigPins.SDCARD_bus.checkConflict(bruceConfigPins.gps_bus.rx)) {
        // T-Embed CC1101 and T-Display S3 Touch ties this pin to the NRF24 CS; switch it to input so the GPS
        // UART can drive it.
        pinMode(bruceConfigPins.gps_bus.rx, INPUT);
        rxPinReleased = true;
    }
}

void GPSTracker::restorePins() {
    if (rxPinReleased) {
        if (bruceConfigPins.CC1101_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
            bruceConfigPins.NRF24_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
#if !defined(LITE_VERSION)
            bruceConfigPins.W5500_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
            bruceConfigPins.LoRa_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
#endif
            bruceConfigPins.SDCARD_bus.checkConflict(bruceConfigPins.gps_bus.rx)) {
            // Restore the original board state after leaving the GPS app s
            // o the radio/other peripherals behave as expected
            pinMode(bruceConfigPins.gps_bus.rx, OUTPUT);
            if (bruceConfigPins.gps_bus.rx == bruceConfigPins.CC1101_bus.cs ||
                bruceConfigPins.gps_bus.rx == bruceConfigPins.NRF24_bus.cs ||
#if !defined(LITE_VERSION)
                bruceConfigPins.gps_bus.rx == bruceConfigPins.W5500_bus.cs ||
                bruceConfigPins.gps_bus.rx == bruceConfigPins.W5500_bus.cs ||
#endif
                bruceConfigPins.gps_bus.rx == bruceConfigPins.SDCARD_bus.cs) {
                // If it is conflicting to an SPI CS pin, keep it HIGH
                digitalWrite(bruceConfigPins.gps_bus.rx, HIGH);
            } else {
                // If it is conflicting with any other SPI pin, keep it LOW
                // Avoids CC1101 Jamming and nRF24 radio to keep enabled
                digitalWrite(bruceConfigPins.gps_bus.rx, LOW);
            }
        }
        rxPinReleased = false;
    }
}
