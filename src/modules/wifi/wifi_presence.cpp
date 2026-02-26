/*
 * WiFi Passive Presence Monitor
 * Sniffs Beacon/ProbeReq/ProbeResp frames and profiles nearby presence.
 */
#include "wifi_presence.h"

#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/wifi/wifi_common.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <Arduino.h>
#include <ctype.h>
#include <globals.h>
#include <pgmspace.h>
#include <time.h>

#define WPM_MAX_MAC 1024
#define WPM_MAX_CLUSTER 96
#define WPM_HOP_MS 2000UL
#define WPM_REDRAW_MS 2000UL
#define WPM_SESSION_GAP (5UL * 60UL * 1000UL)
#define WPM_CROWD_WINDOW 30000UL
#define WPM_CROWD_THR 8
#define WPM_CROWD_DELTA 3
#define WPM_ALERT_SESS 3
#define WPM_CLUSTER_WINDOW (15UL * 60UL * 1000UL)
#define WPM_CLUSTER_RSSI_DELTA 10
#define WPM_NUM_ALERTS 3
#define WPM_LIST_LIMIT 24

#define WPM_FILTER_ALL 0
#define WPM_FILTER_AP 1
#define WPM_FILTER_CLIENT 2
#define WPM_FILTER_RANDOM 3
#define WPM_FILTER_REAL 4

static const uint8_t kWpmCh[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
#define WPM_NUM_CH (sizeof(kWpmCh) / sizeof(kWpmCh[0]))

struct WpmOui {
    uint8_t p[3];
    const char *v;
};

static const WpmOui kOui[] PROGMEM = {
    {{0x00, 0x17, 0xF2}, "Apple"},      {{0xA4, 0xC3, 0x61}, "Apple"},      {{0xAC, 0xBC, 0x32}, "Apple"},
    {{0x3C, 0x15, 0xC2}, "Apple"},      {{0x70, 0xCD, 0x60}, "Apple"},      {{0x18, 0x65, 0x90}, "Apple"},
    {{0x00, 0x23, 0x12}, "Apple"},      {{0xF0, 0xD1, 0xA9}, "Apple"},      {{0x44, 0x00, 0x10}, "Apple"},
    {{0xBC, 0x92, 0x6B}, "Apple"},      {{0x00, 0x17, 0xC9}, "Samsung"},    {{0xA0, 0x07, 0x98}, "Samsung"},
    {{0x50, 0x32, 0x75}, "Samsung"},    {{0xF8, 0x04, 0x2E}, "Samsung"},    {{0x38, 0x2C, 0x4A}, "Samsung"},
    {{0x94, 0x35, 0x0A}, "Samsung"},    {{0x8C, 0x71, 0xF8}, "Samsung"},    {{0xE8, 0x50, 0x8B}, "Samsung"},
    {{0x00, 0x9A, 0xCD}, "Huawei"},     {{0x28, 0x6E, 0xD4}, "Huawei"},     {{0xC8, 0xD1, 0x5E}, "Huawei"},
    {{0x48, 0xFB, 0x7E}, "Huawei"},     {{0xA4, 0x99, 0x47}, "Huawei"},     {{0x28, 0xE3, 0x1F}, "Xiaomi"},
    {{0x34, 0xCE, 0x00}, "Xiaomi"},     {{0x58, 0x44, 0x98}, "Xiaomi"},     {{0xAC, 0xC1, 0xEE}, "Xiaomi"},
    {{0x64, 0x09, 0x80}, "Xiaomi"},     {{0x90, 0x4E, 0x91}, "Xiaomi"},     {{0xF4, 0xF5, 0xDB}, "Google"},
    {{0x3C, 0x5A, 0xB4}, "Google"},     {{0xA4, 0x77, 0x33}, "Google"},     {{0x7C, 0x2E, 0xBD}, "Google"},
    {{0x20, 0xDF, 0xB9}, "Google"},     {{0x00, 0x1E, 0x65}, "Intel"},      {{0x8C, 0x70, 0x5A}, "Intel"},
    {{0xA4, 0x34, 0xD9}, "Intel"},      {{0x00, 0x21, 0x6B}, "Intel"},      {{0x34, 0x13, 0xE8}, "Intel"},
    {{0xF8, 0x34, 0x41}, "Intel"},      {{0x44, 0x37, 0xE6}, "Intel"},      {{0x6C, 0x3B, 0x6B}, "TP-Link"},
    {{0x50, 0xC7, 0xBF}, "TP-Link"},    {{0xB0, 0x95, 0x75}, "TP-Link"},    {{0x18, 0xD6, 0xC7}, "TP-Link"},
    {{0xD8, 0x0D, 0x17}, "TP-Link"},    {{0x60, 0xE3, 0x27}, "TP-Link"},    {{0x24, 0x6F, 0x28}, "Espressif"},
    {{0xEC, 0x64, 0xC9}, "Espressif"},  {{0x30, 0xAE, 0xA4}, "Espressif"},  {{0xA4, 0xCF, 0x12}, "Espressif"},
    {{0xCC, 0x50, 0xE3}, "Espressif"},  {{0x70, 0x03, 0x9F}, "Espressif"},  {{0xB8, 0x27, 0xEB}, "RasPi"},
    {{0xDC, 0xA6, 0x32}, "RasPi"},      {{0xE4, 0x5F, 0x01}, "RasPi"},      {{0x28, 0xCD, 0xC1}, "RasPi"},
    {{0x40, 0xB4, 0xCD}, "Amazon"},     {{0x68, 0x37, 0xE9}, "Amazon"},     {{0x84, 0xD6, 0xD0}, "Amazon"},
    {{0xFC, 0x65, 0xDE}, "Amazon"},     {{0x00, 0x50, 0xF2}, "Microsoft"},  {{0x28, 0x18, 0x78}, "Microsoft"},
    {{0x7C, 0x1E, 0x52}, "Microsoft"},  {{0xD4, 0xA3, 0x3D}, "Microsoft"},  {{0xAC, 0xE2, 0xD3}, "OnePlus"},
    {{0x18, 0x26, 0x49}, "OnePlus"},    {{0x8C, 0x0B, 0x4A}, "OnePlus"},    {{0x00, 0x1A, 0x80}, "Sony"},
    {{0xD8, 0xD4, 0x3C}, "Sony"},       {{0xF4, 0x4E, 0xFD}, "Sony"},       {{0x00, 0x1C, 0x62}, "LG"},
    {{0xA8, 0x1B, 0x5A}, "LG"},         {{0x64, 0xBC, 0x0C}, "LG"},         {{0x00, 0x27, 0x13}, "Lenovo"},
    {{0x88, 0x70, 0x8C}, "Lenovo"},     {{0x90, 0x0B, 0x1C}, "Lenovo"},     {{0x00, 0x1A, 0x92}, "ASUS"},
    {{0x1C, 0x87, 0x2C}, "ASUS"},       {{0x10, 0x7B, 0x44}, "ASUS"},       {{0xD8, 0xCB, 0x8A}, "Dell"},
    {{0x18, 0x03, 0x73}, "Dell"},       {{0x3C, 0xD9, 0x2B}, "HP"},         {{0xA0, 0x36, 0x9F}, "HP"},
    {{0x00, 0x1B, 0xD4}, "Cisco"},      {{0x58, 0xAC, 0x78}, "Cisco"},      {{0x2C, 0x3F, 0x38}, "Cisco"},
    {{0x00, 0x14, 0x6C}, "Netgear"},    {{0xA0, 0x21, 0xB7}, "Netgear"},    {{0xC0, 0x3F, 0x0E}, "Netgear"},
    {{0xCC, 0x3D, 0x82}, "Qualcomm"},   {{0x8C, 0xC8, 0xF4}, "Qualcomm"},   {{0x00, 0x0C, 0xE7}, "MediaTek"},
    {{0x00, 0x08, 0x22}, "MediaTek"},   {{0x44, 0x18, 0xFD}, "MediaTek"},   {{0x24, 0xA4, 0x3C}, "Ubiquiti"},
    {{0x04, 0x18, 0xD6}, "Ubiquiti"},   {{0x68, 0x72, 0x51}, "Ubiquiti"},   {{0x00, 0x25, 0x9C}, "Linksys"},
    {{0xC0, 0x56, 0x27}, "Linksys"},
};
#define WPM_OUI_N (sizeof(kOui) / sizeof(kOui[0]))

struct WpmVendorCacheEntry {
    uint32_t prefix24;
    char vendor[40];
    bool valid;
};

enum WpmVendorDbFs : uint8_t {
    WPM_DB_NONE = 0,
    WPM_DB_SD,
    WPM_DB_LITTLEFS
};

static WpmVendorCacheEntry s_vendorCache[64];
static uint8_t s_vendorCacheHead = 0;
static bool s_vendorDbReady = false;
static WpmVendorDbFs s_vendorDbFs = WPM_DB_NONE;
static char s_vendorDbPath[40] = {0};
static const char *WPM_VENDOR_UNKNOWN = "Unknown";

static const char *wpmVendorBuiltin(const uint8_t *mac) {
    WpmOui row;
    for (size_t i = 0; i < WPM_OUI_N; i++) {
        memcpy_P(&row, &kOui[i], sizeof(row));
        if (mac[0] == row.p[0] && mac[1] == row.p[1] && mac[2] == row.p[2]) return row.v;
    }
    return WPM_VENDOR_UNKNOWN;
}

#define WPM_FL_AP 0x01
#define WPM_FL_CLIENT 0x02
#define WPM_FL_RAND 0x04
#define WPM_FL_REAL 0x08
#define WPM_FL_ALERTED 0x10

struct WpmMac {
    uint8_t mac[6];
    uint8_t flags;
    int8_t rssi;
    uint32_t firstSeen;
    uint32_t lastSeen;
    uint32_t sessStart;
    uint32_t pktCount;
    uint8_t sessions;
    uint8_t clusterId;
    uint8_t clusterSessCredit;
    uint32_t totalSec;
    uint16_t avgVisitSec;
    uint16_t chanMask;
    uint16_t hourBits;
    char ssid[20];
};

struct WpmCluster {
    uint32_t ieHash;
    uint16_t macCount;
    uint16_t sessions;
    int8_t avgRssi;
    uint32_t firstSeen;
    uint32_t lastSeen;
    uint8_t alerted;
};

static WpmMac *s_macs = nullptr;
static WpmCluster *s_clusters = nullptr;
static uint16_t s_macCount = 0;
static uint8_t s_clusterCount = 0;
static volatile uint32_t s_totalPkts = 0;
static volatile uint8_t s_chanIdx = 0;
static volatile bool s_running = false;
static char s_alertBuf[WPM_NUM_ALERTS][52];
static uint8_t s_alertHead = 0;
static uint8_t s_alertCount = 0;

static bool wpmAllocTables() {
    if (s_macs != nullptr && s_clusters != nullptr) return true;

    const size_t macBytes = sizeof(WpmMac) * WPM_MAX_MAC;
    const size_t clusterBytes = sizeof(WpmCluster) * WPM_MAX_CLUSTER;

    s_macs = (WpmMac *)heap_caps_malloc(macBytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!s_macs) s_macs = (WpmMac *)heap_caps_malloc(macBytes, MALLOC_CAP_8BIT);
    if (!s_macs) s_macs = (WpmMac *)malloc(macBytes);

    s_clusters = (WpmCluster *)heap_caps_malloc(clusterBytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!s_clusters) s_clusters = (WpmCluster *)heap_caps_malloc(clusterBytes, MALLOC_CAP_8BIT);
    if (!s_clusters) s_clusters = (WpmCluster *)malloc(clusterBytes);

    if (!s_macs || !s_clusters) {
        if (s_macs) heap_caps_free(s_macs);
        if (s_clusters) heap_caps_free(s_clusters);
        s_macs = nullptr;
        s_clusters = nullptr;
        return false;
    }
    return true;
}

static void wpmFreeTables() {
    if (s_macs) heap_caps_free(s_macs);
    if (s_clusters) heap_caps_free(s_clusters);
    s_macs = nullptr;
    s_clusters = nullptr;
}

static uint8_t wpmBits(uint16_t v) {
    uint8_t c = 0;
    while (v) {
        c += (v & 1);
        v >>= 1;
    }
    return c;
}

static uint8_t wpmPresenceBin(uint32_t nowMs) {
    time_t nowTs = time(nullptr);
    if (nowTs > 1700000000) {
        struct tm info;
        localtime_r(&nowTs, &info);
        return (uint8_t)((info.tm_hour / 2) % 12);
    }
    return (uint8_t)((nowMs / (2UL * 3600UL * 1000UL)) % 12);
}

static bool wpmInvalidMac(const uint8_t *mac) {
    bool all0 = true;
    bool allF = true;
    for (uint8_t i = 0; i < 6; i++) {
        all0 = all0 && (mac[i] == 0x00);
        allF = allF && (mac[i] == 0xFF);
    }
    return all0 || allF;
}

static void wpmPushAlert(const char *msg) {
    strncpy(s_alertBuf[s_alertHead], msg, sizeof(s_alertBuf[0]) - 1);
    s_alertBuf[s_alertHead][sizeof(s_alertBuf[0]) - 1] = '\0';
    s_alertHead = (s_alertHead + 1) % WPM_NUM_ALERTS;
    if (s_alertCount < WPM_NUM_ALERTS) s_alertCount++;
}

static const char *wpmLastAlert() {
    if (s_alertCount == 0) return nullptr;
    uint8_t idx = (uint8_t)((s_alertHead + WPM_NUM_ALERTS - 1) % WPM_NUM_ALERTS);
    return s_alertBuf[idx];
}

static const char *wpmVendorCacheLookup(uint32_t prefix24) {
    for (size_t i = 0; i < (sizeof(s_vendorCache) / sizeof(s_vendorCache[0])); i++) {
        if (s_vendorCache[i].valid && s_vendorCache[i].prefix24 == prefix24) return s_vendorCache[i].vendor;
    }
    return nullptr;
}

static const char *wpmVendorCacheStore(uint32_t prefix24, const char *vendor) {
    WpmVendorCacheEntry &slot =
        s_vendorCache[s_vendorCacheHead % (sizeof(s_vendorCache) / sizeof(s_vendorCache[0]))];
    slot.prefix24 = prefix24;
    strncpy(slot.vendor, vendor ? vendor : WPM_VENDOR_UNKNOWN, sizeof(slot.vendor) - 1);
    slot.vendor[sizeof(slot.vendor) - 1] = '\0';
    slot.valid = true;
    s_vendorCacheHead++;
    return slot.vendor;
}

static bool wpmParsePrefix24(const String &line, uint32_t &outPrefix24) {
    String s = line;
    s.trim();
    if (s.length() < 6) return false;
    if (s[0] == '#') return false;
    if (s.startsWith("Registry") || s.startsWith("Assignment") || s.startsWith("MA-L")
        || s.startsWith("OUI")) {
        return false;
    }

    char hex[7] = {0};
    uint8_t n = 0;
    for (size_t i = 0; i < s.length() && n < 6; i++) {
        char c = s[i];
        if (isxdigit((unsigned char)c)) {
            hex[n++] = c;
            continue;
        }
        if (c == '-' || c == ':' || c == '.') continue;
        if (n == 0 && (c == '"' || c == '\'' || isspace((unsigned char)c))) continue;
        if (n < 6) return false;
    }
    if (n != 6) return false;

    outPrefix24 = (uint32_t)strtoul(hex, nullptr, 16);
    return true;
}

static String wpmTrimVendor(String in) {
    in.trim();
    if (in.length() >= 2 && in[0] == '"' && in[in.length() - 1] == '"') {
        in = in.substring(1, in.length() - 1);
    }
    in.replace("\"\"", "\"");
    in.trim();
    return in;
}

static String wpmExtractVendorName(const String &line) {
    int p = line.indexOf("(base 16)");
    if (p >= 0) return wpmTrimVendor(line.substring(p + 9));

    p = line.indexOf("(hex)");
    if (p >= 0) return wpmTrimVendor(line.substring(p + 5));

    int firstComma = line.indexOf(',');
    if (firstComma >= 0) {
        int i = firstComma + 1;
        while (i < line.length() && isspace((unsigned char)line[i])) i++;
        if (i < line.length() && line[i] == '"') {
            int end = line.indexOf('"', i + 1);
            if (end > i) return wpmTrimVendor(line.substring(i + 1, end));
        }
        int secondComma = line.indexOf(',', i);
        if (secondComma < 0) secondComma = line.length();
        return wpmTrimVendor(line.substring(i, secondComma));
    }

    int start = 0;
    while (start < line.length() && !isspace((unsigned char)line[start])) start++;
    while (start < line.length() && isspace((unsigned char)line[start])) start++;
    return wpmTrimVendor(line.substring(start));
}

static void wpmInitVendorDatabase() {
    if (s_vendorDbReady) return;
    s_vendorDbReady = true;
    s_vendorDbFs = WPM_DB_NONE;
    s_vendorDbPath[0] = '\0';

    static const char *paths[] = {
        "/Bruce/wifi/oui.txt", "/Bruce/oui.txt", "/wifi/oui.txt", "/oui.txt",
        "/Bruce/wifi/oui.csv", "/Bruce/oui.csv", "/wifi/oui.csv", "/oui.csv",
    };

    bool sdReady = setupSdCard();
    if (sdReady) {
        for (size_t i = 0; i < (sizeof(paths) / sizeof(paths[0])); i++) {
            if (SD.exists(paths[i])) {
                s_vendorDbFs = WPM_DB_SD;
                strncpy(s_vendorDbPath, paths[i], sizeof(s_vendorDbPath) - 1);
                s_vendorDbPath[sizeof(s_vendorDbPath) - 1] = '\0';
                return;
            }
        }
    }

    if (littleFsMounted) {
        for (size_t i = 0; i < (sizeof(paths) / sizeof(paths[0])); i++) {
            if (LittleFS.exists(paths[i])) {
                s_vendorDbFs = WPM_DB_LITTLEFS;
                strncpy(s_vendorDbPath, paths[i], sizeof(s_vendorDbPath) - 1);
                s_vendorDbPath[sizeof(s_vendorDbPath) - 1] = '\0';
                return;
            }
        }
    }
}

static bool wpmLookupVendorFromDb(uint32_t prefix24, char *vendorOut, size_t vendorOutLen) {
    if (vendorOutLen == 0) return false;
    wpmInitVendorDatabase();
    if (s_vendorDbFs == WPM_DB_NONE || s_vendorDbPath[0] == '\0') return false;

    FS *fs = (s_vendorDbFs == WPM_DB_SD) ? (FS *)&SD : (FS *)&LittleFS;
    File file = fs->open(s_vendorDbPath, FILE_READ);
    if (!file) return false;

    bool found = false;
    while (file.available()) {
        String line = file.readStringUntil('\n');
        uint32_t linePrefix = 0;
        if (!wpmParsePrefix24(line, linePrefix) || linePrefix != prefix24) continue;

        String vendor = wpmExtractVendorName(line);
        if (vendor.isEmpty()) continue;

        strncpy(vendorOut, vendor.c_str(), vendorOutLen - 1);
        vendorOut[vendorOutLen - 1] = '\0';
        found = true;
        break;
    }
    file.close();
    return found;
}

static const char *wpmVendor(const uint8_t *mac) {
    uint32_t prefix24 = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];

    const char *cached = wpmVendorCacheLookup(prefix24);
    if (cached) return cached;

    char vendor[40] = {0};
    if (wpmLookupVendorFromDb(prefix24, vendor, sizeof(vendor))) {
        return wpmVendorCacheStore(prefix24, vendor);
    }

    return wpmVendorCacheStore(prefix24, wpmVendorBuiltin(mac));
}

static uint32_t wpmIEHash(const uint8_t *frame, uint16_t len, uint16_t offset) {
    static const uint8_t tags[] = {1, 45, 48, 50, 70, 127, 191};
    uint32_t h = 2166136261UL;
    for (uint16_t pos = offset; pos + 2 <= len;) {
        uint8_t tag = frame[pos];
        uint8_t elen = frame[pos + 1];
        if (pos + 2 + elen > len) break;
        bool take = false;
        for (uint8_t i = 0; i < sizeof(tags); i++) {
            if (tag == tags[i]) {
                take = true;
                break;
            }
        }
        if (take) {
            h ^= tag;
            h *= 16777619UL;
            uint8_t copyLen = elen < 16 ? elen : 16;
            for (uint8_t i = 0; i < copyLen; i++) {
                h ^= frame[pos + 2 + i];
                h *= 16777619UL;
            }
        }
        pos += (uint16_t)(2 + elen);
    }
    return h;
}

static void wpmExtractSsid(const uint8_t *frame, uint16_t len, uint16_t offset, char *out, size_t outLen) {
    for (uint16_t pos = offset; pos + 2 <= len;) {
        uint8_t tag = frame[pos];
        uint8_t elen = frame[pos + 1];
        if (pos + 2 + elen > len) break;
        if (tag == 0 && elen > 0) {
            size_t n = (size_t)elen < (outLen - 1) ? elen : (outLen - 1);
            memcpy(out, &frame[pos + 2], n);
            out[n] = '\0';
            return;
        }
        pos += (uint16_t)(2 + elen);
    }
}

static uint32_t wpmActiveSec(const WpmMac &e, uint32_t now) {
    uint32_t total = e.totalSec;
    if (e.sessions == 0 || e.sessStart == 0) return total;
    uint32_t end = now;
    if (now - e.lastSeen > WPM_SESSION_GAP) end = e.lastSeen;
    if (end > e.sessStart) total += (end - e.sessStart) / 1000UL;
    return total;
}

static WpmMac *wpmFindOrInsert(const uint8_t *mac, uint32_t now) {
    for (uint16_t i = 0; i < s_macCount; i++) {
        if (memcmp(s_macs[i].mac, mac, 6) == 0) return &s_macs[i];
    }
    if (s_macCount < WPM_MAX_MAC) {
        WpmMac &e = s_macs[s_macCount++];
        memset(&e, 0, sizeof(e));
        memcpy(e.mac, mac, 6);
        e.firstSeen = now;
        return &e;
    }
    uint16_t evict = 0;
    for (uint16_t i = 1; i < s_macCount; i++) {
        if (s_macs[i].lastSeen < s_macs[evict].lastSeen) evict = i;
    }
    WpmMac &e = s_macs[evict];
    memset(&e, 0, sizeof(e));
    memcpy(e.mac, mac, 6);
    e.firstSeen = now;
    return &e;
}

static uint8_t wpmFindOrAddCluster(uint32_t hash, int8_t rssi, uint32_t now) {
    for (uint8_t i = 0; i < s_clusterCount; i++) {
        int d = s_clusters[i].avgRssi - rssi;
        if (d < 0) d = -d;
        if (s_clusters[i].ieHash == hash && d <= WPM_CLUSTER_RSSI_DELTA
            && (now - s_clusters[i].lastSeen) <= WPM_CLUSTER_WINDOW) {
            return (uint8_t)(i + 1);
        }
    }
    uint8_t id = s_clusterCount;
    if (s_clusterCount < WPM_MAX_CLUSTER) s_clusterCount++;
    else {
        id = 0;
        for (uint8_t i = 1; i < s_clusterCount; i++) {
            if (s_clusters[i].lastSeen < s_clusters[id].lastSeen) id = i;
        }
    }
    WpmCluster &c = s_clusters[id];
    memset(&c, 0, sizeof(c));
    c.ieHash = hash;
    c.avgRssi = rssi;
    c.firstSeen = now;
    c.lastSeen = now;
    return (uint8_t)(id + 1);
}

static const char *wpmTierStr(const WpmMac &e, uint32_t now) {
    if (e.flags & WPM_FL_AP) return "access-point";
    uint32_t active = wpmActiveSec(e, now);
    if (e.flags & WPM_FL_RAND) {
        if (e.sessions >= 8 || active >= 4UL * 3600UL) return "rnd-resident";
        if (e.sessions >= 5) return "rnd-regular";
        if (e.sessions >= 3) return "rnd-return";
        return "rnd-transient";
    }
    if (e.sessions >= 8 || active >= 4UL * 3600UL) return "resident";
    if (e.sessions >= 5) return "regular";
    if (e.sessions >= 3) return "returning";
    if (now - e.lastSeen <= 120000UL) return "online";
    return "transient";
}

static const char *wpmClassStr(const WpmMac &e, uint32_t now) {
    if (e.flags & WPM_FL_AP) return "access-point";
    uint8_t chSeen = wpmBits(e.chanMask);
    uint32_t active = wpmActiveSec(e, now);
    if (active >= 7200UL && chSeen <= 2) return "stationary";
    if (e.sessions >= 4 && chSeen >= 3) return "mobile-regular";
    if (e.sessions >= 3) return "regular-client";
    if (e.avgVisitSec > 0 && e.avgVisitSec < 120) return "passerby";
    if (now - e.lastSeen <= 120000UL) return "online-client";
    return "visitor";
}

static const char *wpmPatternStr(const WpmMac &e) {
    uint16_t b = e.hourBits;
    if (b == 0) return "unknown";
    if (wpmBits(b) >= 9) return "all-day";
    uint8_t morning = ((b >> 3) & 1) + ((b >> 4) & 1) + ((b >> 5) & 1);
    uint8_t day = ((b >> 6) & 1) + ((b >> 7) & 1) + ((b >> 8) & 1);
    uint8_t evening = ((b >> 9) & 1) + ((b >> 10) & 1);
    uint8_t night = ((b >> 11) & 1) + ((b >> 0) & 1) + ((b >> 1) & 1) + ((b >> 2) & 1);
    if (day >= morning && day >= evening && day >= night) return "day";
    if (evening >= morning && evening >= day && evening >= night) return "evening";
    if (night >= morning && night >= day && night >= evening) return "night";
    return "morning";
}

static String wpmMacToString(const uint8_t *mac) {
    char s[18];
    snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(s);
}

static String wpmChannelMaskString(uint16_t mask) {
    String out = "";
    for (uint8_t ch = 1; ch <= 13; ch++) {
        if ((mask & (uint16_t)(1U << (ch - 1))) == 0) continue;
        if (!out.isEmpty()) out += ",";
        out += String(ch);
    }
    return out.isEmpty() ? String("-") : out;
}

static bool wpmMatchFilter(const WpmMac &e, uint8_t filter) {
    if (filter == WPM_FILTER_AP) return (e.flags & WPM_FL_AP) != 0;
    if (filter == WPM_FILTER_CLIENT) return (e.flags & WPM_FL_CLIENT) != 0;
    if (filter == WPM_FILTER_RANDOM) return (e.flags & WPM_FL_RAND) != 0;
    if (filter == WPM_FILTER_REAL) return (e.flags & WPM_FL_REAL) != 0;
    return true;
}

static uint8_t wpmCollectRecentByFilter(uint16_t *out, uint8_t maxOut, uint8_t filter) {
    if (maxOut == 0) return 0;

    uint8_t count = 0;
    for (uint16_t i = 0; i < s_macCount; i++) {
        const WpmMac &e = s_macs[i];
        if (!wpmMatchFilter(e, filter)) continue;

        uint8_t pos = count;
        while (pos > 0 && s_macs[out[pos - 1]].lastSeen < e.lastSeen) pos--;
        if (pos >= maxOut) continue;

        if (count < maxOut) count++;
        for (int16_t j = (int16_t)count - 1; j > (int16_t)pos; j--) out[j] = out[j - 1];
        out[pos] = i;
    }

    return count;
}

static void wpmShowMacDetails(uint16_t idx) {
    if (idx >= s_macCount) return;

    const WpmMac &e = s_macs[idx];
    uint32_t now = millis();

    drawMainBorderWithTitle("MAC Details");

    String macStr = wpmMacToString(e.mac);
    padprintln(macStr);

    const char *kind = (e.flags & WPM_FL_AP) ? "AP"
                       : (e.flags & WPM_FL_RAND) ? "Client (rnd)"
                                                 : "Client (real)";
    padprintln(String("Type: ") + kind);
    padprintln(String("Vendor: ") + wpmVendor(e.mac));

    char buf[64];
    snprintf(buf, sizeof(buf), "RSSI:%ddBm Pkts:%lu", e.rssi, (unsigned long)e.pktCount);
    padprintln(buf);
    snprintf(buf, sizeof(buf), "Sess:%u Active:%lus", e.sessions, (unsigned long)wpmActiveSec(e, now));
    padprintln(buf);
    snprintf(buf, sizeof(buf), "AvgVisit:%us Cluster:U#%u", e.avgVisitSec, e.clusterId);
    padprintln(buf);

    padprintln(String("Tier: ") + wpmTierStr(e, now));
    padprintln(String("Class: ") + wpmClassStr(e, now));
    padprintln(String("Pattern: ") + wpmPatternStr(e));
    padprintln(String("Channels: ") + wpmChannelMaskString(e.chanMask));
    if (e.flags & WPM_FL_AP) padprintln(String("SSID: ") + (e.ssid[0] ? String(e.ssid) : String("<hidden>")));

    padprintln("");
    padprintln("[Any key] Back");
    while (!check(AnyKeyPress)) vTaskDelay(10 / portTICK_PERIOD_MS);
}

static void wpmOpenMacList(uint8_t filter, const char *title) {
    uint16_t idx[WPM_LIST_LIMIT];
    uint8_t count = wpmCollectRecentByFilter(idx, WPM_LIST_LIMIT, filter);
    if (count == 0) {
        displayInfo("No heard MACs yet", true);
        return;
    }

    std::vector<Option> menu;
    menu.reserve(count + 1);

    for (uint8_t i = 0; i < count; i++) {
        uint16_t macIdx = idx[i];
        const WpmMac &e = s_macs[macIdx];
        char label[72];

        if (e.flags & WPM_FL_AP) {
            snprintf(
                label,
                sizeof(label),
                "[AP] %02X:%02X:%02X.. %ddBm",
                e.mac[0],
                e.mac[1],
                e.mac[2],
                e.rssi
            );
        } else if (e.flags & WPM_FL_RAND) {
            snprintf(
                label,
                sizeof(label),
                "[R] %02X:%02X:%02X.. %ddBm",
                e.mac[0],
                e.mac[1],
                e.mac[2],
                e.rssi
            );
        } else {
            snprintf(
                label,
                sizeof(label),
                "[C] %02X:%02X:%02X.. %ddBm",
                e.mac[0],
                e.mac[1],
                e.mac[2],
                e.rssi
            );
        }

        menu.push_back({label, [macIdx]() { wpmShowMacDetails(macIdx); }});
    }
    menu.push_back({"Back", []() {}});
    loopOptions(menu, MENU_TYPE_SUBMENU, title);
}

static void wpmShowMonitorStats() {
    drawMainBorderWithTitle("Presence Stats");
    wpmInitVendorDatabase();

    uint16_t aps = 0, rnd = 0, clients = 0, real = 0;
    for (uint16_t i = 0; i < s_macCount; i++) {
        if (s_macs[i].flags & WPM_FL_AP) aps++;
        if (s_macs[i].flags & WPM_FL_CLIENT) clients++;
        if (s_macs[i].flags & WPM_FL_RAND) rnd++;
        if (s_macs[i].flags & WPM_FL_REAL) real++;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "Devices: %u", s_macCount);
    padprintln(buf);
    snprintf(buf, sizeof(buf), "APs: %u  Clients: %u", aps, clients);
    padprintln(buf);
    snprintf(buf, sizeof(buf), "Randomized: %u  Real: %u", rnd, real);
    padprintln(buf);
    snprintf(buf, sizeof(buf), "Clusters: %u", s_clusterCount);
    padprintln(buf);
    snprintf(buf, sizeof(buf), "Packets: %lu", (unsigned long)s_totalPkts);
    padprintln(buf);
    snprintf(buf, sizeof(buf), "Current channel: %u", kWpmCh[s_chanIdx]);
    padprintln(buf);
    if (s_vendorDbFs == WPM_DB_NONE) padprintln("Vendor DB: built-in only");
    else padprintln(String("Vendor DB: ") + s_vendorDbPath);

    const char *last = wpmLastAlert();
    if (last) padprintln(String("Last alert: ") + last);
    else padprintln("Last alert: -");

    padprintln("");
    padprintln("[Any key] Back");
    while (!check(AnyKeyPress)) vTaskDelay(10 / portTICK_PERIOD_MS);
}

static void wpmOpenInfoMenu() {
    bool wasRunning = s_running;
    if (wasRunning) esp_wifi_set_promiscuous(false);

    std::vector<Option> menu;
    menu.reserve(7);
    menu.push_back({"Summary", []() { wpmShowMonitorStats(); }});
    menu.push_back({"Heard MACs (all)", []() { wpmOpenMacList(WPM_FILTER_ALL, "All Heard MACs"); }});
    menu.push_back({"Heard AP MACs", []() { wpmOpenMacList(WPM_FILTER_AP, "AP MACs"); }});
    menu.push_back({"Heard Client MACs", []() { wpmOpenMacList(WPM_FILTER_CLIENT, "Client MACs"); }});
    menu.push_back({"Randomized MACs", []() { wpmOpenMacList(WPM_FILTER_RANDOM, "Randomized MACs"); }});
    menu.push_back({"Real MACs", []() { wpmOpenMacList(WPM_FILTER_REAL, "Real MACs"); }});
    menu.push_back({"Back", []() {}});
    loopOptions(menu, MENU_TYPE_SUBMENU, "Presence Info");

    if (wasRunning) esp_wifi_set_promiscuous(true);
}

static void wpmCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
    (void)type;
    if (!s_running) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = pkt->payload;
    uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
    if (len < 24) return;

    uint8_t fType = (frame[0] & 0x0C) >> 2;
    uint8_t fSub = (frame[0] & 0xF0) >> 4;
    if (fType != 0 || (fSub != 0x08 && fSub != 0x04 && fSub != 0x05)) return;

    const uint8_t *src = frame + 10;
    if (wpmInvalidMac(src)) return;

    uint32_t now = millis();
    s_totalPkts++;

    WpmMac *e = wpmFindOrInsert(src, now);
    bool isAP = (fSub == 0x08 || fSub == 0x05);
    bool isRand = (src[0] & 0x02) != 0;

    e->flags |= isAP ? WPM_FL_AP : WPM_FL_CLIENT;
    if (isRand) {
        e->flags |= WPM_FL_RAND;
        e->flags &= (uint8_t)~WPM_FL_REAL;
    } else {
        e->flags |= WPM_FL_REAL;
        e->flags &= (uint8_t)~WPM_FL_RAND;
    }

    e->rssi = (e->pktCount == 0) ? (int8_t)pkt->rx_ctrl.rssi : (int8_t)((e->rssi * 3 + pkt->rx_ctrl.rssi) / 4);

    bool newSession = false;
    if (e->sessions == 0) {
        e->sessions = 1;
        e->sessStart = now;
        newSession = true;
    } else if (e->lastSeen > 0 && (now - e->lastSeen) > WPM_SESSION_GAP) {
        uint32_t dur = (e->lastSeen > e->sessStart) ? (e->lastSeen - e->sessStart) / 1000UL : 0;
        e->totalSec += dur;
        e->avgVisitSec = e->avgVisitSec == 0 ? (uint16_t)dur : (uint16_t)((e->avgVisitSec * 7 + dur) / 8);
        if (e->sessions < 255) e->sessions++;
        e->sessStart = now;
        newSession = true;
    }

    e->lastSeen = now;
    e->pktCount++;

    uint8_t ch = pkt->rx_ctrl.channel;
    if (ch < 1 || ch > 13) ch = kWpmCh[s_chanIdx];
    e->chanMask |= (uint16_t)(1U << (ch - 1));
    e->hourBits |= (uint16_t)(1U << wpmPresenceBin(now));

    uint16_t ieOffset = (fSub == 0x04) ? 24 : 36;
    if (isAP && e->ssid[0] == '\0' && len > ieOffset) wpmExtractSsid(frame, len, ieOffset, e->ssid, sizeof(e->ssid));

    if (isRand && len > ieOffset) {
        uint32_t hash = wpmIEHash(frame, len, ieOffset);
        if (hash != 2166136261UL) {
            uint8_t cid = wpmFindOrAddCluster(hash, e->rssi, now);
            WpmCluster &c = s_clusters[cid - 1];
            c.lastSeen = now;
            c.avgRssi = (int8_t)((c.avgRssi * 3 + e->rssi) / 4);
            if (e->clusterId != cid) {
                e->clusterId = cid;
                e->clusterSessCredit = 0;
                c.macCount++;
            }
            if (newSession && e->clusterSessCredit < e->sessions) {
                e->clusterSessCredit = e->sessions;
                c.sessions++;
                if (!c.alerted && c.sessions >= WPM_ALERT_SESS) {
                    c.alerted = 1;
                    char a[52];
                    snprintf(a, sizeof(a), "Recurring cluster U#%02u (%u MACs)", cid, c.macCount);
                    wpmPushAlert(a);
                }
            }
        }
    }

    if (!isRand && newSession && e->sessions >= WPM_ALERT_SESS && !(e->flags & WPM_FL_ALERTED)) {
        e->flags |= WPM_FL_ALERTED;
        char a[52];
        snprintf(a, sizeof(a), "Returning device %02X:%02X:%02X", src[0], src[1], src[2]);
        wpmPushAlert(a);
    }
}

static void wpmDraw() {
    drawMainBorderWithTitle("Presence Monitor");

    uint16_t aps = 0, rnd = 0, clients = 0;
    for (uint16_t i = 0; i < s_macCount; i++) {
        if (s_macs[i].flags & WPM_FL_AP) aps++;
        if (s_macs[i].flags & WPM_FL_CLIENT) clients++;
        if (s_macs[i].flags & WPM_FL_RAND) rnd++;
    }

    char buf[56];
    snprintf(buf, sizeof(buf), "Dev:%u AP:%u Cli:%u", s_macCount, aps, clients);
    padprintln(buf);
    snprintf(buf, sizeof(buf), "Rnd:%u Cls:%u Ch:%u Pk:%lu", rnd, s_clusterCount, kWpmCh[s_chanIdx], (unsigned long)s_totalPkts);
    padprintln(buf);

    const char *last = wpmLastAlert();
    if (last) {
        snprintf(buf, sizeof(buf), "!%.50s", last);
        padprintln(buf);
    } else {
        padprintln("No alerts");
    }

    const WpmMac *focus = nullptr;
    for (uint16_t i = 0; i < s_macCount; i++) {
        if (!focus || s_macs[i].lastSeen > focus->lastSeen) focus = &s_macs[i];
    }
    if (focus) {
        uint32_t now = millis();
        snprintf(buf, sizeof(buf), "Tier:%s Pat:%s", wpmTierStr(*focus, now), wpmPatternStr(*focus));
        padprintln(buf);
        snprintf(buf, sizeof(buf), "Class:%s", wpmClassStr(*focus, now));
        padprintln(buf);
    } else {
        padprintln("Tier:- Pat:-");
        padprintln("Class:-");
    }

    padprintln("--- Recent ---");
    uint8_t shown = 0;
    uint32_t now = millis();
    for (int16_t i = (int16_t)s_macCount - 1; i >= 0 && shown < 6; i--) {
        const WpmMac &e = s_macs[i];
        if (now - e.lastSeen > 60000UL) continue;
        if (e.flags & WPM_FL_AP) snprintf(buf, sizeof(buf), "[AP] %.14s %ddBm", e.ssid[0] ? e.ssid : "hidden", e.rssi);
        else if (e.flags & WPM_FL_RAND) {
            if (e.clusterId) snprintf(buf, sizeof(buf), "U#%02u %s %ddBm", e.clusterId, wpmTierStr(e, now), e.rssi);
            else snprintf(buf, sizeof(buf), "%02X:%02X rnd %ddBm", e.mac[4], e.mac[5], e.rssi);
        } else snprintf(buf, sizeof(buf), "%02X:%02X %s %s", e.mac[4], e.mac[5], wpmVendor(e.mac), wpmTierStr(e, now));
        padprintln(buf);
        shown++;
    }
    if (shown == 0) padprintln("Waiting for management frames...");
    padprintln("[Sel] details  [Esc/Back] exit");
}

void wifi_presence_monitor() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    if (esp_wifi_stop() == ESP_OK) {
        esp_wifi_deinit();
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    if (!wpmAllocTables()) {
        displayError("Presence monitor: no memory", true);
        returnToMenu = true;
        return;
    }

    memset(s_macs, 0, sizeof(WpmMac) * WPM_MAX_MAC);
    memset(s_clusters, 0, sizeof(WpmCluster) * WPM_MAX_CLUSTER);
    memset(s_alertBuf, 0, sizeof(s_alertBuf));
    memset(s_vendorCache, 0, sizeof(s_vendorCache));
    s_macCount = 0;
    s_clusterCount = 0;
    s_totalPkts = 0;
    s_chanIdx = 0;
    s_alertHead = 0;
    s_alertCount = 0;
    s_vendorCacheHead = 0;
    s_vendorDbReady = false;
    s_vendorDbFs = WPM_DB_NONE;
    s_vendorDbPath[0] = '\0';
    s_running = false;

    bool initOk = true;
    do {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            err = nvs_flash_init();
        }
        if (err != ESP_OK) {
            initOk = false;
            break;
        }

        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            initOk = false;
            break;
        }

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable = false;
        if (esp_wifi_init(&cfg) != ESP_OK) {
            initOk = false;
            break;
        }
        if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK || esp_wifi_set_mode(WIFI_MODE_NULL) != ESP_OK
            || esp_wifi_start() != ESP_OK) {
            initOk = false;
            break;
        }

        wifi_promiscuous_filter_t filter;
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
        if (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK || esp_wifi_set_promiscuous_rx_cb(wpmCallback) != ESP_OK
            || esp_wifi_set_channel(kWpmCh[0], WIFI_SECOND_CHAN_NONE) != ESP_OK
            || esp_wifi_set_promiscuous(true) != ESP_OK) {
            initOk = false;
            break;
        }
    } while (false);

    if (!initOk) {
        s_running = false;
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(nullptr);
        esp_wifi_stop();
        esp_wifi_deinit();
        wpmFreeTables();
        displayError("Presence monitor init failed", true);
        returnToMenu = true;
        return;
    }

    s_running = true;
    uint32_t lastHop = millis();
    uint32_t lastDraw = 0;
    uint32_t lastCrowd = millis();
    uint16_t prevRecent = 0;

    wpmDraw();

    while (1) {
        if (check(EscPress)) break;
        if (check(SelPress)) {
            wpmOpenInfoMenu();
            wpmDraw();
            lastDraw = millis();
        }
        uint32_t now = millis();

        if (now - lastHop >= WPM_HOP_MS) {
            esp_wifi_set_promiscuous(false);
            s_chanIdx = (uint8_t)((s_chanIdx + 1) % WPM_NUM_CH);
            esp_wifi_set_channel(kWpmCh[s_chanIdx], WIFI_SECOND_CHAN_NONE);
            esp_wifi_set_promiscuous(true);
            lastHop = now;
        }

        if (now - lastCrowd >= WPM_CROWD_WINDOW) {
            uint16_t recent = 0;
            for (uint16_t i = 0; i < s_macCount; i++) {
                if (now - s_macs[i].lastSeen <= WPM_CROWD_WINDOW) recent++;
            }
            if (recent >= WPM_CROWD_THR && recent >= (uint16_t)(prevRecent + WPM_CROWD_DELTA)) {
                char a[52];
                snprintf(a, sizeof(a), "Crowd spike: %u active devices", recent);
                wpmPushAlert(a);
            }
            prevRecent = recent;
            lastCrowd = now;
        }

        if (now - lastDraw >= WPM_REDRAW_MS) {
            wpmDraw();
            lastDraw = now;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    s_running = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_stop();
    esp_wifi_deinit();
    wpmFreeTables();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    returnToMenu = true;
}
