#pragma once

#include <Arduino.h>

// Supported GPS backends
enum gps_source_t : uint8_t {
    GPS_SOURCE_LEGACY = 0,
    GPS_SOURCE_CASIC = 1,
};

// Leap-second status
struct gps_leap_second_t {
    bool valid = false;
    int8_t current = 0;     // utcLs
    int8_t pending = 0;     // utcLsf
    uint8_t day = 0;        // scheduled day (UTC)
    uint8_t month = 0;
    uint16_t year = 0;
};

typedef struct {
    bool fix_valid = false;
    uint8_t fix_type = 0; // 0 = no fix, 2 = 2D, 3 = 3D
    double lat_deg = 0;   // +N, -S
    double lon_deg = 0;   // +E, -W
    double alt_m = 0;     // height above MSL
    uint8_t sats_used = 0;
    float hdop = 0;
    float vdop = 0;
    float pdop = 0;
    float speed_mps = 0;
    float speed_kph = 0;
    float course_deg = 0;
    uint8_t hour = 0, min = 0, sec = 0;
    uint16_t msec = 0;
    uint8_t day = 0, month = 0;
    uint16_t year = 0;
} gps_fix_t;

typedef enum {
    GNSS_SYS_GPS,
    GNSS_SYS_BDS,
    GNSS_SYS_GLONASS,
    GNSS_SYS_GALILEO,
    GNSS_SYS_QZSS,
    GNSS_SYS_SBAS,
    GNSS_SYS_UNKNOWN
} gnss_system_t;

typedef struct {
    gnss_system_t system = GNSS_SYS_UNKNOWN;
    uint16_t svid = 0;        // NMEA SVID
    uint16_t prn = 0;         // optional PRN
    uint8_t elevation_deg = 0;
    uint16_t azimuth_deg = 0;
    uint8_t cn0_dbhz = 0;
    bool used_in_fix = false;
} gnss_sat_t;

#ifndef CASIC_MAX_SATS
#define CASIC_MAX_SATS 64
#endif

typedef struct {
    gnss_sat_t sats[CASIC_MAX_SATS];
    uint8_t count = 0;
} gnss_sat_view_t;

typedef enum {
    GPS_ANT_UNKNOWN,
    GPS_ANT_OK,
    GPS_ANT_OPEN,
    GPS_ANT_SHORT
} gps_ant_status_t;

typedef struct {
    char manufacturer[16] = {0}; // MA=
    char ic[32] = {0};           // IC=
    char sw[32] = {0};           // SW=
    char build_time[32] = {0};   // TB=
    char mode[8] = {0};          // MO=
    char customer_id[16] = {0};  // CI=
} gps_casic_info_t;

typedef struct {
    uint8_t gga = 1;
    uint8_t gll = 0;
    uint8_t gsa = 1;
    uint8_t gsv = 1;
    uint8_t rmc = 1;
    uint8_t vtg = 0;
    uint8_t zda = 0;
    uint8_t ant = 1;
    uint8_t dhv = 0;
    uint8_t lps = 0;
    uint8_t utc = 0;
    uint8_t gst = 0;
    uint8_t tim = 0;
} gps_casic_nmea_cfg_t;

struct gps_casic_state_t {
    gps_fix_t fix;
    gnss_sat_view_t sats;
    gps_ant_status_t ant_status = GPS_ANT_UNKNOWN;
    gps_leap_second_t leap;
    bool fix_updated = false;
    bool seen_bytes = false;
};
