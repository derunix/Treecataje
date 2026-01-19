#include "gps_casic_ui.h"

#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/settings.h"
#include "core/utils.h"
#include "gps_casic.h"
#include "gps_provider.h"
#include <globals.h>

static const char *sys_to_str(gnss_system_t sys) {
    switch (sys) {
    case GNSS_SYS_GPS: return "GPS";
    case GNSS_SYS_BDS: return "BDS";
    case GNSS_SYS_GLONASS: return "GLN";
    case GNSS_SYS_GALILEO: return "GAL";
    case GNSS_SYS_QZSS: return "QZS";
    case GNSS_SYS_SBAS: return "SBA";
    default: return "UNK";
    }
}

static const char *ant_to_str(gps_ant_status_t st) {
    switch (st) {
    case GPS_ANT_OK: return "OK";
    case GPS_ANT_OPEN: return "OPEN";
    case GPS_ANT_SHORT: return "SHORT";
    default: return "UNKNOWN";
    }
}

static const char *fix_to_str(uint8_t fixType) {
    switch (fixType) {
    case 2: return "2D";
    case 3: return "3D";
    default: return "NO";
    }
}

GPSCasicSatView::GPSCasicSatView() { setup(); }
GPSCasicSatView::~GPSCasicSatView() {
    ioExpander.turnPinOnOff(IO_EXP_GPS, LOW);
#ifdef USE_BOOST
    PPM.disableOTG();
#endif
}

void GPSCasicSatView::setup() {
    ioExpander.turnPinOnOff(IO_EXP_GPS, HIGH);
#ifdef USE_BOOST
    PPM.enableOTG();
#endif

    if (bruceConfig.gpsSource != BruceConfig::GPS_SOURCE_CASIC) {
        displayError("Select CASIC as GPS Source", true);
        return;
    }

    gps_provider_begin();
    loop();
}

void GPSCasicSatView::loop() {
    uint8_t page = 0;
    const uint8_t rowsPerPage = 8;
    returnToMenu = false;
    uint32_t lastDraw = 0;
    uint8_t lastPage = 0xFF;
    while (1) {
        gps_provider_tick();
        const gps_fix_t &fix = gps_provider_get_fix();
        const gnss_sat_view_t &view = gps_provider_get_sat_view();
        uint8_t sysCount[7] = {0};
        for (uint8_t i = 0; i < view.count; ++i) {
            gnss_system_t s = view.sats[i].system;
            if (s >= 0 && s < 7) sysCount[s]++;
        }
        int pages = view.count == 0 ? 1 : (view.count + rowsPerPage - 1) / rowsPerPage;
        if (page >= pages) page = pages - 1;

        bool needRedraw = (millis() - lastDraw > 500) || gps_provider_fix_updated(true) ||
                          page != lastPage || gps_provider_seen_bytes(false);
        if (needRedraw) {
            drawMainBorderWithTitle("Satellites (CASIC)");
            padprintf(2, "Fix: %s  Used:%2d  Vis:%2d\n", fix_to_str(fix.fix_type), fix.sats_used, view.count);
            padprintf(2, "HDOP: %.1f  PDOP: %.1f  VDOP: %.1f\n", fix.hdop, fix.pdop, fix.vdop);
            padprintf(2, "Ant: %s\n", ant_to_str(gps_provider_get_antenna_status()));
            padprintf(
                2,
                "GPS:%d BDS:%d GLN:%d GAL:%d QZ:%d SBA:%d\n",
                sysCount[GNSS_SYS_GPS],
                sysCount[GNSS_SYS_BDS],
                sysCount[GNSS_SYS_GLONASS],
                sysCount[GNSS_SYS_GALILEO],
                sysCount[GNSS_SYS_QZSS],
                sysCount[GNSS_SYS_SBAS]
            );
            padprintln("");
            padprintln("SYS  SVID EL  AZ  C/N USED", 2);

            uint8_t start = page * rowsPerPage;
            uint8_t end = min<uint8_t>(view.count, start + rowsPerPage);
            for (uint8_t i = start; i < end; ++i) {
                const auto &s = view.sats[i];
                char bar[17] = {0};
                uint8_t len = min<uint8_t>(15, s.cn0_dbhz / 3);
                for (uint8_t b = 0; b < len; ++b) bar[b] = '#';
                bar[len] = 0;
                padprintf(
                    2,
                    "%c%-3s %3d %3d %3d %3d %s\n",
                    s.used_in_fix ? '*' : ' ',
                    sys_to_str(s.system),
                    s.svid,
                    s.elevation_deg,
                    s.azimuth_deg,
                    s.cn0_dbhz,
                    bar
                );
            }

            padprintf(2, "Page %d/%d   ESC=Back\n", page + 1, pages);
            lastDraw = millis();
            lastPage = page;
        }

        if (check(EscPress) || returnToMenu) return;
        if (check(NextPress) || check(NextPagePress)) page = (page + 1) % pages;
        if (check(PrevPress) || check(PrevPagePress)) page = (page == 0 ? pages - 1 : page - 1);
        vTaskDelay(120 / portTICK_PERIOD_MS);
    }
}

GPSCasicInfoScreen::GPSCasicInfoScreen() { setup(); }
GPSCasicInfoScreen::~GPSCasicInfoScreen() {
    ioExpander.turnPinOnOff(IO_EXP_GPS, LOW);
#ifdef USE_BOOST
    PPM.disableOTG();
#endif
}

void GPSCasicInfoScreen::setup() {
    ioExpander.turnPinOnOff(IO_EXP_GPS, HIGH);
#ifdef USE_BOOST
    PPM.enableOTG();
#endif
    if (bruceConfig.gpsSource != BruceConfig::GPS_SOURCE_CASIC) {
        displayError("Select CASIC as GPS Source", true);
        return;
    }

    gps_provider_begin();
    loop();
}

void GPSCasicInfoScreen::loop() {
    gps_casic_info_t info{};
    gps_casic_query_info(&info, 800);

    uint32_t lastRefresh = 0;
    returnToMenu = false;
    while (1) {
        gps_provider_tick();
        bool needUpdate = gps_provider_seen_bytes(false) || (millis() - lastRefresh > 1000);

        if (needUpdate) {
            drawMainBorderWithTitle("CASIC Info");
            padprintf(2, "Manufacturer: %s\n", info.manufacturer);
            padprintf(2, "IC:          %s\n", info.ic);
            padprintf(2, "SW:          %s\n", info.sw);
            padprintf(2, "Build Time:  %s\n", info.build_time);
            padprintf(2, "Mode:        %s\n", info.mode);
            padprintf(2, "Customer ID: %s\n", info.customer_id);
            padprintf(2, "Antenna:     %s\n", ant_to_str(gps_provider_get_antenna_status()));
            padprintln("");
            padprintln("Next=Refresh   ESC=Back", 2);
            lastRefresh = millis();
        }

        if (check(EscPress) || returnToMenu) return;
        if (check(NextPress) || check(SelPress)) {
            gps_casic_query_info(&info, 800);
            lastRefresh = 0;
        }
        vTaskDelay(150 / portTICK_PERIOD_MS);
    }
}

GPSCasicConfigMenu::GPSCasicConfigMenu() { setup(); }
GPSCasicConfigMenu::~GPSCasicConfigMenu() {
    ioExpander.turnPinOnOff(IO_EXP_GPS, LOW);
#ifdef USE_BOOST
    PPM.disableOTG();
#endif
}

void GPSCasicConfigMenu::setup() {
    ioExpander.turnPinOnOff(IO_EXP_GPS, HIGH);
#ifdef USE_BOOST
    PPM.enableOTG();
#endif
    if (bruceConfig.gpsSource != BruceConfig::GPS_SOURCE_CASIC) {
        displayError("Select CASIC as GPS Source", true);
        return;
    }

    gps_provider_begin();
    loop();
}

void GPSCasicConfigMenu::loop() {
    bool exitMenu = false;
    while (!exitMenu) {
        options = {
            {"Apply Baudrate", [=]() { apply_baudrate(); }},
            {"Apply Update Hz", [=]() { apply_update_rate(); }},
            {"NMEA Profile", [=]() { apply_nmea_profile(); }},
            {"Mute ANT TXT", [=]() { apply_mute_ant_txt(); }},
            {"System Mode", [=]() { apply_system_mode(); }},
            {"Save Config", [=]() { apply_save(); }},
            {"Restart (Cold)", [=]() { apply_reset(); }},
            {"Standby 10s", [=]() { apply_standby(); }},
            {"Mask SBAS Off", [=]() { apply_sv_mask(); }},
            {"Back", [&, this]() { exitMenu = true; }},
        };
        loopOptions(options, MENU_TYPE_SUBMENU, "CASIC Config");
    }
}

void GPSCasicConfigMenu::apply_system_mode() {
    options = {
        {"GPS",            [=]() { gps_casic_set_system_mode(1); }},
        {"BDS",            [=]() { gps_casic_set_system_mode(2); }},
        {"GPS+BDS",        [=]() { gps_casic_set_system_mode(3); }},
        {"GLONASS",        [=]() { gps_casic_set_system_mode(4); }},
        {"GPS+GLONASS",    [=]() { gps_casic_set_system_mode(5); }},
        {"BDS+GLONASS",    [=]() { gps_casic_set_system_mode(6); }},
        {"GPS+BDS+GLONASS",[=]() { gps_casic_set_system_mode(7); }},
    };
    loopOptions(options, MENU_TYPE_SUBMENU, "System Mode");
}

void GPSCasicConfigMenu::apply_baudrate() {
    int applied = bruceConfig.gpsBaudrate;
    if (gps_casic_set_baudrate(gps_casic_baud_code_from_value(bruceConfig.gpsBaudrate), applied)) {
        bruceConfig.setGpsBaudrate(applied);
        displaySuccess("Baud set to " + String(applied), true);
    } else displayError("Failed to set baud", true);
}

void GPSCasicConfigMenu::apply_update_rate() {
    if (gps_casic_set_update_rate_ms(bruceConfig.gpsUpdateRateMs)) {
        displaySuccess("Rate applied", true);
    } else displayError("Invalid rate", true);
}

void GPSCasicConfigMenu::apply_nmea_profile() {
    gps_casic_nmea_cfg_t cfg{};
    cfg.gga = 1;
    cfg.gsa = 1;
    cfg.gsv = 1;
    cfg.rmc = 1;
    cfg.ant = 1;
    cfg.utc = 1;
    if (gps_casic_configure_nmea(cfg)) displaySuccess("NMEA profile set", true);
    else displayError("Failed NMEA config", true);
}

void GPSCasicConfigMenu::apply_mute_ant_txt() {
    gps_casic_nmea_cfg_t cfg{};
    cfg.gga = 1;
    cfg.gsa = 1;
    cfg.gsv = 1;
    cfg.rmc = 1;
    cfg.ant = 0; // disable antenna TXT
    cfg.utc = 1;
    if (gps_casic_configure_nmea(cfg)) displaySuccess("ANT TXT disabled", true);
    else displayError("Mute failed", true);
}

void GPSCasicConfigMenu::apply_save() {
    if (gps_casic_save_config()) displaySuccess("Saved to flash", true);
    else displayError("Save failed", true);
}

void GPSCasicConfigMenu::apply_reset() {
    options = {
        {"Hot",    [=]() { gps_casic_reset(0); }},
        {"Warm",   [=]() { gps_casic_reset(1); }},
        {"Cold",   [=]() { gps_casic_reset(2); }},
        {"Factory",[=]() { gps_casic_reset(3); }},
    };
    loopOptions(options, MENU_TYPE_SUBMENU, "Restart Mode");
}

void GPSCasicConfigMenu::apply_standby() {
    if (gps_casic_standby(10)) displaySuccess("Standby 10s", true);
    else displayError("Standby failed", true);
}

void GPSCasicConfigMenu::apply_sv_mask() {
    // Quick toggle: disable SBAS (sys_id 4) by setting mask 0
    if (gps_casic_set_sv_mask(4, 0x00000000)) displaySuccess("SBAS disabled", true);
    else displayError("Mask failed", true);
}
