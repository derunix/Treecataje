#include "gps_casic.h"
#include "gps_types.h"

#include <Arduino.h>
#include <HardwareSerial.h>
#include "core/configPins.h"
#include <string.h>
#include <vector>
#include <deque>
#include <esp_log.h>

// Simple UART-backed CASIC GNSS driver with line-based NMEA parsing.

// CRITICAL FIX: Use shared HardwareSerial pointer instead of static instance
static HardwareSerial *casicSerial = nullptr;
static bool casicRunning = false;

static constexpr size_t RX_BUF_SIZE = 1024;
static char rxBuf[RX_BUF_SIZE];
static size_t rxHead = 0;
static size_t rxTail = 0;
static std::deque<String> nmeaLog;

static gps_casic_state_t state;
static gps_casic_info_t cachedInfo;
static bool infoUpdated = false;
static const bool kIgnoreAntennaOpen = true; // some boards use only internal patch antenna
static bool statusLogEnabled = false; // default off to avoid log spam

// Utility forward declarations
static bool extract_line(String &out);
static bool handle_sentence(const String &line);
static gnss_system_t system_from_talker(const String &talker);
static gnss_system_t system_from_svid(uint16_t svid);
static bool parse_time_field(const String &src, uint8_t &h, uint8_t &m, uint8_t &s, uint16_t &ms);
static bool parse_date_field(const String &src, uint8_t &day, uint8_t &month, uint16_t &year);
static bool parse_latlon(const String &coord, char hemi, double &out);
static gnss_sat_t *upsert_sat(gnss_system_t sys, uint16_t svid);
static void clear_system_sats(gnss_system_t sys);
static void mark_used_svid(uint16_t svid, gnss_system_t hint);
static void handle_txt_payload(const String &text);
static const char *TAG = "CASIC";

uint8_t nmea_checksum(const char *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; ++i) crc ^= static_cast<uint8_t>(data[i]);
    return crc;
}

uint8_t gps_casic_baud_code_from_value(int baud) {
    switch (baud) {
    case 4800: return 0;
    case 9600: return 1;
    case 19200: return 2;
    case 38400: return 3;
    case 57600: return 4;
    case 115200: return 5;
    default: return 1;
    }
}

int gps_casic_baud_from_code(uint8_t code) {
    switch (code) {
    case 0: return 4800;
    case 1: return 9600;
    case 2: return 19200;
    case 3: return 38400;
    case 4: return 57600;
    case 5: return 115200;
    default: return 9600;
    }
}

bool gps_casic_init(int baudrate, const BruceConfigPins &pins, HardwareSerial **sharedSerial) {
    rxHead = rxTail = 0;
    state = gps_casic_state_t();
    cachedInfo = gps_casic_info_t();
    infoUpdated = false;

    // Use shared serial instance if provided, otherwise create new one
    if (sharedSerial && *sharedSerial) {
        casicSerial = *sharedSerial;
        Serial.println("[CASIC] Using shared UART2 instance");
    } else {
        if (!casicSerial) {
            casicSerial = new HardwareSerial(2);
        }
        Serial.println("[CASIC] Using dedicated UART2 instance");
    }

    casicSerial->end();
    casicSerial->begin(baudrate, SERIAL_8N1, pins.gps_bus.rx, pins.gps_bus.tx);
    casicRunning = true;
    Serial.printf("[CASIC] Initialized on UART2 (RX:%d, TX:%d, baud:%d)\n",
                  pins.gps_bus.rx, pins.gps_bus.tx, baudrate);
    return true;
}

void gps_casic_deinit() {
    if (!casicRunning) return;
    if (casicSerial) {
        casicSerial->end();
    }
    casicRunning = false;
    Serial.println("[CASIC] Deinitialized");
    // Note: We don't delete casicSerial as it may be shared
}

bool gps_casic_seen_bytes(bool clear) {
    bool seen = state.seen_bytes;
    if (clear) state.seen_bytes = false;
    return seen;
}

static void push_nmea_log(const String &line) {
    nmeaLog.push_back(line);
    const size_t kMax = 200;
    while (nmeaLog.size() > kMax) nmeaLog.pop_front();
}

void gps_casic_get_recent_nmea(std::vector<String> &out, size_t maxLines) {
    out.clear();
    if (maxLines == 0) return;
    size_t start = nmeaLog.size() > maxLines ? nmeaLog.size() - maxLines : 0;
    for (size_t i = start; i < nmeaLog.size(); ++i) out.push_back(nmeaLog[i]);
}

void gps_casic_enable_status_log(bool enable) { statusLogEnabled = enable; }
bool gps_casic_status_log_enabled() { return statusLogEnabled; }

void gps_casic_tick() {
    if (!casicRunning || !casicSerial) return;

    while (casicSerial->available()) {
        state.seen_bytes = true;
        char c = static_cast<char>(casicSerial->read());
        rxBuf[rxHead] = c;
        rxHead = (rxHead + 1) % RX_BUF_SIZE;
        if (rxHead == rxTail) { // drop oldest byte on overflow
            rxTail = (rxTail + 1) % RX_BUF_SIZE;
        }
    }

    String line;
    while (extract_line(line)) {
        // NMEA decoded below; raw dump suppressed to keep logs readable
        handle_sentence(line);
    }

    // Human-friendly periodic status log (disabled by default; enable via gps_log)
    static uint32_t lastLogMs = 0;
    uint32_t now = millis();
    if (statusLogEnabled && now - lastLogMs > 1500) {
        lastLogMs = now;
        const gps_fix_t &f = state.fix;
        gnss_sat_view_t view = state.sats; // copy for sorting and counts
        std::vector<gnss_sat_t> sats(view.sats, view.sats + view.count);
        std::sort(sats.begin(), sats.end(), [](const gnss_sat_t &a, const gnss_sat_t &b) { return a.cn0_dbhz > b.cn0_dbhz; });

        auto sys_name = [](gnss_system_t s) {
            switch (s) {
            case GNSS_SYS_GPS: return "GPS";
            case GNSS_SYS_BDS: return "BDS";
            case GNSS_SYS_GLONASS: return "GLN";
            case GNSS_SYS_GALILEO: return "GAL";
            case GNSS_SYS_QZSS: return "QZ";
            case GNSS_SYS_SBAS: return "SB";
            default: return "UNK";
            }
        };
        auto cn0_label = [](uint8_t cn0) {
            if (cn0 >= 40) return "excellent";
            if (cn0 >= 30) return "good";
            if (cn0 >= 25) return "ok";
            if (cn0 >= 20) return "weak";
            return "very weak";
        };

        const char *antTxt = (state.ant_status == GPS_ANT_OK)    ? "antenna OK"
                             : (state.ant_status == GPS_ANT_OPEN)  ? "antenna OPEN"
                             : (state.ant_status == GPS_ANT_SHORT) ? "antenna SHORT"
                                                                   : "antenna ?";
        const char *fixTxt = f.fix_valid ? "YES" : "NO";
        uint8_t sysCount[7] = {0};
        for (uint8_t i = 0; i < view.count; ++i) {
            gnss_system_t s = view.sats[i].system;
            if (s >= 0 && s < 7) sysCount[s]++;
        }

        ESP_LOGI(TAG, "[Log] === GNSS summary ===");
        ESP_LOGI(TAG, "[Log] Fix: %s (mode %u), used %u / visible %u", fixTxt, f.fix_type, f.sats_used, view.count);
        if (f.fix_valid) {
            ESP_LOGI(TAG, "[Log] Position: lat %.6f lon %.6f alt %.1f m", f.lat_deg, f.lon_deg, f.alt_m);
            ESP_LOGI(TAG, "[Log] Speed %.1f km/h  Course %.1f deg", f.speed_kph, f.course_deg);
        } else {
            ESP_LOGI(TAG, "[Log] Position: unknown (no fix yet)");
        }
        ESP_LOGI(TAG, "[Log] Systems: GPS %u, BDS %u, GLN %u, GAL %u, QZSS %u, SBAS %u", sysCount[GNSS_SYS_GPS], sysCount[GNSS_SYS_BDS], sysCount[GNSS_SYS_GLONASS], sysCount[GNSS_SYS_GALILEO], sysCount[GNSS_SYS_QZSS], sysCount[GNSS_SYS_SBAS]);
        ESP_LOGI(TAG, "[Log] Antenna: %s", antTxt);
        ESP_LOGI(TAG, "[Log] --- Satellites (sorted by signal) ---");
        ESP_LOGI(TAG, "[Log] SYS SVID  C/N0(dBHz)  Signal        El  Az  Used");
        for (const auto &s : sats) {
            ESP_LOGI(TAG, "[Log] %-3s %4u  %3u (%-9s) %3u  %3u   %s",
                     sys_name(s.system),
                     s.svid,
                     s.cn0_dbhz,
                     cn0_label(s.cn0_dbhz),
                     s.elevation_deg,
                     s.azimuth_deg,
                     s.used_in_fix ? "YES" : " no");
        }
    }
}

bool gps_casic_has_fix() { return state.fix.fix_valid; }

bool gps_casic_fix_updated(bool clear) {
    bool updated = state.fix_updated;
    if (clear) state.fix_updated = false;
    return updated;
}

const gps_fix_t &gps_casic_get_fix() { return state.fix; }

const gnss_sat_view_t &gps_casic_get_sat_view() { return state.sats; }

gps_casic_state_t gps_casic_get_state() { return state; }

gps_ant_status_t gps_casic_get_antenna_status() { return state.ant_status; }

// Command helper: payload without '$'/'*'
static bool send_payload(const String &payload) {
    if (!casicSerial) return false;

    char csBuf[3] = {0};
    uint8_t cs = nmea_checksum(payload.c_str(), payload.length());
    snprintf(csBuf, sizeof(csBuf), "%02X", cs);
    ESP_LOGI(TAG, "TX: $%s*%s", payload.c_str(), csBuf);
    casicSerial->print("$");
    casicSerial->print(payload);
    casicSerial->print("*");
    casicSerial->print(csBuf);
    casicSerial->print("\r\n");
    casicSerial->flush();
    return true;
}

bool gps_casic_set_baudrate(uint8_t code, int &appliedBaud) {
    if (!casicSerial) return false;

    appliedBaud = gps_casic_baud_from_code(code);
    String payload = "PCAS01," + String(code);
    if (!send_payload(payload)) return false;
    delay(80);
    casicSerial->updateBaudRate(appliedBaud);
    return true;
}

bool gps_casic_set_update_rate_ms(uint16_t ms) {
    // Allowed: 100, 200, 250, 500, 1000
    const uint16_t allowed[] = {100, 200, 250, 500, 1000};
    bool valid = false;
    for (auto v : allowed) {
        if (ms == v) {
            valid = true;
            break;
        }
    }
    if (!valid) return false;
    String payload = "PCAS02," + String(ms);
    return send_payload(payload);
}

bool gps_casic_configure_nmea(const gps_casic_nmea_cfg_t &cfg) {
    String payload = "PCAS03,";
    payload += String(cfg.gga) + "," + String(cfg.gll) + "," + String(cfg.gsa) + "," + String(cfg.gsv) + "," +
               String(cfg.rmc) + "," + String(cfg.vtg) + "," + String(cfg.zda) + "," + String(cfg.ant) + "," +
               String(cfg.dhv) + "," + String(cfg.lps) + ",0,0," + String(cfg.utc) + "," + String(cfg.gst) +
               ",0,0,0," + String(cfg.tim);
    return send_payload(payload);
}

bool gps_casic_set_system_mode(uint8_t mode) {
    if (mode < 1 || mode > 7) return false;
    return send_payload("PCAS04," + String(mode));
}

bool gps_casic_set_nmea_version(uint8_t ver) { return send_payload("PCAS05," + String(ver)); }

bool gps_casic_reset(uint8_t mode) { return send_payload("PCAS10," + String(mode)); }

bool gps_casic_standby(uint16_t seconds) { return send_payload("PCAS12," + String(seconds)); }

bool gps_casic_set_sv_mask(uint8_t sys_id, uint32_t mask_low, uint32_t mask_high) {
    char buf[17] = {0};
    if (mask_high) snprintf(buf, sizeof(buf), "%08lX%08lX", static_cast<unsigned long>(mask_high), static_cast<unsigned long>(mask_low));
    else snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(mask_low));
    String payload = "PCAS15," + String(sys_id) + "," + String(buf);
    return send_payload(payload);
}

bool gps_casic_save_config() { return send_payload("PCAS00"); }

bool gps_casic_query_info(gps_casic_info_t *out, uint32_t timeoutMs) {
    if (!out) return false;
    memset(out, 0, sizeof(gps_casic_info_t));
    infoUpdated = false;
    cachedInfo = gps_casic_info_t();

    const uint8_t queries[] = {0, 1, 2, 3};
    for (uint8_t q : queries) {
        send_payload("PCAS06," + String(q));
        delay(20);
    }

    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        gps_casic_tick();
        if (infoUpdated) break;
        delay(10);
    }

    if (infoUpdated) {
        *out = cachedInfo;
        return true;
    }
    return false;
}

static bool extract_line(String &out) {
    if (rxHead == rxTail) return false;

    String line;
    while (rxTail != rxHead) {
        char c = rxBuf[rxTail];
        rxTail = (rxTail + 1) % RX_BUF_SIZE;
        if (c == '\n') break;
        if (c == '\r') continue;
        line += c;
        if (line.length() > 150) { // avoid runaway lines
            line = "";
            break;
        }
    }

    if (line.length() == 0) return false;
    push_nmea_log(line);
    out = line;
    return true;
}

static bool handle_sentence(const String &line) {
    if (line.length() < 7) return false; // too short
    if (line.charAt(0) != '$') return false;

    int starIndex = line.indexOf('*');
    if (starIndex < 0) return false;

    String checksumStr = line.substring(starIndex + 1);
    uint8_t expected = static_cast<uint8_t>(strtoul(checksumStr.c_str(), nullptr, 16));
    uint8_t calc = nmea_checksum(line.c_str() + 1, starIndex - 1);
    if (expected != calc) return false;

    String payload = line.substring(1, starIndex);
    int comma = payload.indexOf(',');
    String talker = payload.substring(0, 2);
    String type = comma >= 0 ? payload.substring(2, comma) : payload.substring(2);
    String fieldsStr = comma >= 0 ? payload.substring(comma + 1) : "";

    std::vector<String> fields;
    int start = 0;
    while (start <= fieldsStr.length()) {
        int pos = fieldsStr.indexOf(',', start);
        if (pos < 0) pos = fieldsStr.length();
        fields.push_back(fieldsStr.substring(start, pos));
        start = pos + 1;
    }

    auto field = [&](size_t idx) -> String {
        if (idx >= fields.size()) return "";
        return fields[idx];
    };

    if (type.startsWith("GGA")) {
        if (fields.size() >= 9) {
            uint8_t h = 0, m = 0, s = 0;
            uint16_t ms = 0;
            parse_time_field(field(0), h, m, s, ms);
            double lat = 0, lon = 0;
            parse_latlon(field(1), field(2).isEmpty() ? 'N' : field(2).charAt(0), lat);
            parse_latlon(field(3), field(4).isEmpty() ? 'E' : field(4).charAt(0), lon);
            int quality = field(5).toInt();
            state.fix.sats_used = static_cast<uint8_t>(field(6).toInt());
            state.fix.hdop = field(7).toFloat();
            state.fix.alt_m = field(8).toDouble();

            state.fix.lat_deg = lat;
            state.fix.lon_deg = lon;
            state.fix.hour = h;
            state.fix.min = m;
            state.fix.sec = s;
            state.fix.msec = ms;
            state.fix.fix_valid = quality > 0;
            state.fix_updated = true;
        }
    } else if (type.startsWith("RMC")) {
        if (fields.size() >= 9) {
            uint8_t h = 0, m = 0, s = 0;
            uint16_t ms = 0;
            parse_time_field(field(0), h, m, s, ms);
            char status = field(1).isEmpty() ? 'V' : field(1).charAt(0);
            double lat = 0, lon = 0;
            parse_latlon(field(2), field(3).isEmpty() ? 'N' : field(3).charAt(0), lat);
            parse_latlon(field(4), field(5).isEmpty() ? 'E' : field(5).charAt(0), lon);
            float knots = field(6).toFloat();
            float course = field(7).toFloat();
            parse_date_field(field(8), state.fix.day, state.fix.month, state.fix.year);

            state.fix.lat_deg = lat;
            state.fix.lon_deg = lon;
            state.fix.speed_mps = knots * 0.514444f;
            state.fix.speed_kph = knots * 1.852f;
            state.fix.course_deg = course;
            state.fix.hour = h;
            state.fix.min = m;
            state.fix.sec = s;
            state.fix.msec = ms;
            state.fix.fix_valid = (status == 'A');
            state.fix_updated = true;
        }
    } else if (type.startsWith("GSA")) {
        if (fields.size() >= 15) {
            state.fix.fix_type = static_cast<uint8_t>(field(1).toInt());
            state.fix.fix_valid = state.fix.fix_type >= 2;
            state.fix.pdop = field(fields.size() - 3).toFloat();
            state.fix.hdop = field(fields.size() - 2).toFloat();
            state.fix.vdop = field(fields.size() - 1).toFloat();
            gnss_system_t sys = system_from_talker(talker);
            for (size_t i = 2; i < 14; ++i) {
                uint16_t svid = field(i).toInt();
                if (svid == 0) continue;
                mark_used_svid(svid, sys);
            }
            state.fix_updated = true;
        }
    } else if (type.startsWith("GSV")) {
        if (fields.size() >= 4) {
            int msgNo = field(1).toInt();
            gnss_system_t sys = system_from_talker(talker);
            if (msgNo == 1) clear_system_sats(sys);
            for (size_t idx = 3; idx + 3 < fields.size(); idx += 4) {
                uint16_t svid = field(idx).toInt();
                if (svid == 0) continue;
                gnss_sat_t *sat = upsert_sat(sys, svid);
                if (!sat) continue;
                sat->elevation_deg = static_cast<uint8_t>(field(idx + 1).toInt());
                sat->azimuth_deg = static_cast<uint16_t>(field(idx + 2).toInt());
                sat->cn0_dbhz = static_cast<uint8_t>(field(idx + 3).toInt());
                sat->used_in_fix = false; // will be set by GSA if applicable
            }
        }
    } else if (type.startsWith("VTG")) {
        if (fields.size() >= 7) {
            state.fix.course_deg = field(0).toFloat();
            state.fix.speed_kph = field(6).toFloat();
            state.fix.speed_mps = state.fix.speed_kph / 3.6f;
            state.fix_updated = true;
        }
    } else if (type.startsWith("ZDA")) {
        if (fields.size() >= 6) {
            state.fix.day = static_cast<uint8_t>(field(1).toInt());
            state.fix.month = static_cast<uint8_t>(field(2).toInt());
            state.fix.year = static_cast<uint16_t>(field(3).toInt());
            state.fix_updated = true;
        }
    } else if (type.startsWith("TXT")) {
        if (!fields.empty()) handle_txt_payload(field(fields.size() - 1));
    } else if (type.startsWith("UTC")) { // CASIC proprietary UTC
        if (fields.size() >= 10) {
            // leap info fields near the end: leapValid, utcLs, utcLsf, leapTime(yyyymmdd)
            bool valid = field(6).toInt() == 1;
            state.leap.valid = valid;
            if (valid) {
                state.leap.current = static_cast<int8_t>(field(7).toInt());
                state.leap.pending = static_cast<int8_t>(field(8).toInt());
                String ts = field(9);
                if (ts.length() == 8) {
                    state.leap.year = ts.substring(0, 4).toInt();
                    state.leap.month = ts.substring(4, 6).toInt();
                    state.leap.day = ts.substring(6, 8).toInt();
                }
            }
        }
    }

    return true;
}

static bool parse_time_field(const String &src, uint8_t &h, uint8_t &m, uint8_t &s, uint16_t &ms) {
    if (src.length() < 6) return false;
    h = src.substring(0, 2).toInt();
    m = src.substring(2, 4).toInt();
    s = src.substring(4, 6).toInt();
    ms = 0;
    int dot = src.indexOf('.');
    if (dot >= 0 && dot + 1 < src.length()) {
        String frac = src.substring(dot + 1);
        while (frac.length() < 3) frac += "0";
        ms = frac.substring(0, 3).toInt();
    }
    return true;
}

static bool parse_date_field(const String &src, uint8_t &day, uint8_t &month, uint16_t &year) {
    if (src.length() < 6) return false;
    day = src.substring(0, 2).toInt();
    month = src.substring(2, 4).toInt();
    year = 2000 + src.substring(4, 6).toInt();
    return true;
}

static bool parse_latlon(const String &coord, char hemi, double &out) {
    if (coord.length() < 3) return false;
    int dot = coord.indexOf('.');
    if (dot < 0) return false;
    int degDigits = (dot > 3) ? dot - 2 : dot - 1; // 2 for lat, 3 for lon
    double deg = coord.substring(0, degDigits).toDouble();
    double minutes = coord.substring(degDigits).toDouble();
    out = deg + (minutes / 60.0);
    if (hemi == 'S' || hemi == 'W') out = -out;
    return true;
}

static gnss_system_t system_from_talker(const String &talker) {
    if (talker == "GP") return GNSS_SYS_GPS;
    if (talker == "BD" || talker == "GB") return GNSS_SYS_BDS;
    if (talker == "GL") return GNSS_SYS_GLONASS;
    if (talker == "GA") return GNSS_SYS_GALILEO;
    if (talker == "QZ" || talker == "GQ") return GNSS_SYS_QZSS;
    if (talker == "GN") return GNSS_SYS_UNKNOWN;
    if (talker == "PQ" || talker == "PC") return GNSS_SYS_UNKNOWN;
    return GNSS_SYS_UNKNOWN;
}

static gnss_system_t system_from_svid(uint16_t svid) {
    if (svid >= 1 && svid <= 32) return GNSS_SYS_GPS;
    if (svid >= 65 && svid <= 96) return GNSS_SYS_GLONASS;
    if (svid >= 193 && svid <= 199) return GNSS_SYS_QZSS;
    if (svid >= 201 && svid <= 237) return GNSS_SYS_BDS;
    if (svid >= 301 && svid <= 336) return GNSS_SYS_GALILEO;
    if (svid >= 33 && svid <= 64) return GNSS_SYS_SBAS;
    return GNSS_SYS_UNKNOWN;
}

static gnss_sat_t *upsert_sat(gnss_system_t sys, uint16_t svid) {
    for (uint8_t i = 0; i < state.sats.count; ++i) {
        if (state.sats.sats[i].svid == svid && state.sats.sats[i].system == sys) return &state.sats.sats[i];
    }
    if (state.sats.count >= CASIC_MAX_SATS) return nullptr;
    gnss_sat_t &sat = state.sats.sats[state.sats.count++];
    sat = gnss_sat_t();
    sat.svid = svid;
    sat.system = sys == GNSS_SYS_UNKNOWN ? system_from_svid(svid) : sys;
    return &sat;
}

static void clear_system_sats(gnss_system_t sys) {
    if (sys == GNSS_SYS_UNKNOWN) {
        state.sats.count = 0;
        return;
    }

    uint8_t writeIdx = 0;
    for (uint8_t i = 0; i < state.sats.count; ++i) {
        if (state.sats.sats[i].system == sys) continue;
        if (writeIdx != i) state.sats.sats[writeIdx] = state.sats.sats[i];
        ++writeIdx;
    }
    state.sats.count = writeIdx;
}

static void mark_used_svid(uint16_t svid, gnss_system_t hint) {
    gnss_sat_t *sat = upsert_sat(hint, svid);
    if (sat) sat->used_in_fix = true;
}

static void handle_txt_payload(const String &text) {
    // suppress TXT spam in logs; update internal state only
    String upper = text;
    upper.toUpperCase();
    if (upper.indexOf("ANTENNA OPEN") >= 0) {
        state.ant_status = kIgnoreAntennaOpen ? GPS_ANT_OK : GPS_ANT_OPEN;
    } else if (upper.indexOf("ANTENNA SHORT") >= 0) {
        state.ant_status = GPS_ANT_SHORT;
    } else if (upper.indexOf("ANTENNA OK") >= 0) {
        state.ant_status = GPS_ANT_OK;
    }

    auto update_field = [&](const char *tag, char *dest, size_t maxLen) {
        int pos = text.indexOf(tag);
        if (pos < 0) return;
        String val = text.substring(pos + strlen(tag));
        val.trim();
        strlcpy(dest, val.c_str(), maxLen);
        infoUpdated = true;
    };

    update_field("MA=", cachedInfo.manufacturer, sizeof(cachedInfo.manufacturer));
    update_field("IC=", cachedInfo.ic, sizeof(cachedInfo.ic));
    update_field("SW=", cachedInfo.sw, sizeof(cachedInfo.sw));
    update_field("TB=", cachedInfo.build_time, sizeof(cachedInfo.build_time));
    update_field("MO=", cachedInfo.mode, sizeof(cachedInfo.mode));
    update_field("CI=", cachedInfo.customer_id, sizeof(cachedInfo.customer_id));
}
