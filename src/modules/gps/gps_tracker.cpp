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
#include <TinyGPS++.h>
#include "gps_provider.h"

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

void GPSTracker::end() {
    gps_provider_end();
    returnToMenu = true;
}

void GPSTracker::loop() {
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
                add_coord();
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

void GPSTracker::set_position() {
    double lat = fix.lat_deg;
    double lng = fix.lon_deg;

    if (initial_position_set) distance += TinyGPSPlus::distanceBetween(cur_lat, cur_lng, lat, lng);
    else initial_position_set = true;

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
        returnToMenu = true;
        return;
    }

    if (filename == "") create_filename();

    if (!(*fs).exists("/BruceGPS")) (*fs).mkdir("/BruceGPS");

    bool is_new_file = false;
    if (!(*fs).exists("/BruceGPS/" + filename)) is_new_file = true;
    File file = (*fs).open("/BruceGPS/" + filename, is_new_file ? FILE_WRITE : FILE_APPEND);

    if (!file) {
        padprintln("Failed to open file for writing");
        returnToMenu = true;
        return;
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
