#include "gps_provider.h"

#include "gps_casic.h"
#include <TinyGPS++.h>
#include <globals.h>
#include <HardwareSerial.h>
#include <esp_log.h>

static gps_source_t currentSource = GPS_SOURCE_LEGACY;

// CRITICAL FIX: Use shared HardwareSerial instance instead of two separate ones
// Both legacy and CASIC backends will use the same UART2 instance
static HardwareSerial *gpsSerial = nullptr;

// Legacy backend (TinyGPS++)
static TinyGPSPlus legacyGps;
static gps_fix_t legacyFix;
static bool legacyRunning = false;
static bool legacyFixUpdated = false;
static bool legacySeenBytes = false;
static gnss_sat_view_t emptySatView; // placeholder when using legacy backend

static void legacy_begin() {
    // Initialize shared serial instance if not already created
    if (!gpsSerial) {
        gpsSerial = new HardwareSerial(2);
    }

    gpsSerial->end();
    legacyGps = TinyGPSPlus();
    legacyFix = gps_fix_t();
    legacyFixUpdated = false;
    legacySeenBytes = false;
    gpsSerial->begin(
        bruceConfig.gpsBaudrate, SERIAL_8N1, bruceConfigPins.gps_bus.rx, bruceConfigPins.gps_bus.tx
    );
    legacyRunning = true;
    Serial.printf("[GPS] Legacy backend started on UART2 (RX:%d, TX:%d, baud:%d)\n",
                  bruceConfigPins.gps_bus.rx, bruceConfigPins.gps_bus.tx, bruceConfig.gpsBaudrate);
}

static void legacy_end() {
    if (!legacyRunning) return;
    if (gpsSerial) {
        gpsSerial->end();
    }
    legacyRunning = false;
    Serial.println("[GPS] Legacy backend stopped");
}

static void legacy_tick() {
    if (!legacyRunning || !gpsSerial) return;
    while (gpsSerial->available()) {
        legacySeenBytes = true;
        char c = static_cast<char>(gpsSerial->read());
        legacyGps.encode(c);
    }

    if (legacyGps.location.isUpdated()) {
        legacyFix.lat_deg = legacyGps.location.lat();
        legacyFix.lon_deg = legacyGps.location.lng();
        legacyFix.alt_m = legacyGps.altitude.meters();
        legacyFix.speed_mps = legacyGps.speed.mps();
        legacyFix.speed_kph = legacyGps.speed.kmph();
        legacyFix.course_deg = legacyGps.course.deg();
        legacyFix.sats_used = static_cast<uint8_t>(legacyGps.satellites.value());
        legacyFix.hdop = legacyGps.hdop.hdop();

        legacyFix.hour = legacyGps.time.hour();
        legacyFix.min = legacyGps.time.minute();
        legacyFix.sec = legacyGps.time.second();
        legacyFix.msec = legacyGps.time.centisecond() * 10;
        legacyFix.day = legacyGps.date.day();
        legacyFix.month = legacyGps.date.month();
        legacyFix.year = legacyGps.date.year();

        legacyFix.fix_type = legacyGps.location.isValid() ? 3 : 0;
        legacyFix.fix_valid = legacyGps.location.isValid();
        legacyFixUpdated = true;
    }
}

bool gps_provider_begin() {
    // silence noisy ledc verbose logs from core (e.g. analogWrite)
    esp_log_level_set("ledc", ESP_LOG_ERROR);
    esp_log_level_set("esp32-hal-ledc", ESP_LOG_ERROR);

    // Ensure clean shutdown before switching backends
    gps_provider_end();

    // Small delay to ensure UART is fully released
    delay(50);

    currentSource = static_cast<gps_source_t>(bruceConfig.gpsSource);
    Serial.printf("[GPS] Initializing GPS provider, source=%d\n", currentSource);

    if (currentSource == GPS_SOURCE_CASIC) {
        // Pass shared serial instance to CASIC backend
        if (!gps_casic_init(bruceConfig.gpsBaudrate, bruceConfigPins, &gpsSerial)) {
            Serial.println("[GPS] CASIC init failed, falling back to legacy");
            currentSource = GPS_SOURCE_LEGACY;
            legacy_begin();
        } else {
            gps_casic_set_update_rate_ms(bruceConfig.gpsUpdateRateMs);
            Serial.println("[GPS] CASIC backend initialized successfully");
        }
    } else {
        legacy_begin();
    }

    gpsConnected = true;
    return true;
}

void gps_provider_end() {
    Serial.printf("[GPS] Shutting down GPS provider, source=%d\n", currentSource);

    if (currentSource == GPS_SOURCE_CASIC) {
        gps_casic_deinit();
    } else {
        legacy_end();
    }

    // Note: We don't delete gpsSerial here to allow reuse
    // It will be cleaned up when switching backends or at program end

    gpsConnected = false;
    Serial.println("[GPS] GPS provider shut down");
}

void gps_provider_tick() {
    if (currentSource == GPS_SOURCE_CASIC) gps_casic_tick();
    else legacy_tick();
}

gps_source_t gps_provider_current_source() { return currentSource; }

bool gps_provider_fix_updated(bool clear) {
    if (currentSource == GPS_SOURCE_CASIC) return gps_casic_fix_updated(clear);
    bool updated = legacyFixUpdated;
    if (clear) legacyFixUpdated = false;
    return updated;
}

const gps_fix_t &gps_provider_get_fix() {
    if (currentSource == GPS_SOURCE_CASIC) return gps_casic_get_fix();
    return legacyFix;
}

const gnss_sat_view_t &gps_provider_get_sat_view() {
    if (currentSource == GPS_SOURCE_CASIC) return gps_casic_get_sat_view();
    emptySatView.count = 0;
    return emptySatView;
}

gps_ant_status_t gps_provider_get_antenna_status() {
    if (currentSource == GPS_SOURCE_CASIC) return gps_casic_get_antenna_status();
    return GPS_ANT_UNKNOWN;
}

bool gps_provider_has_fix() {
    if (currentSource == GPS_SOURCE_CASIC) return gps_casic_has_fix();
    return legacyFix.fix_valid;
}

bool gps_provider_seen_bytes(bool clear) {
    if (currentSource == GPS_SOURCE_CASIC) return gps_casic_seen_bytes(clear);
    bool seen = legacySeenBytes;
    if (clear) legacySeenBytes = false;
    return seen;
}
