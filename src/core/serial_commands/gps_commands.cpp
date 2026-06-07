#include "gps_commands.h"

#include "modules/gps/gps_casic.h"
#include "modules/gps/gps_casic_ui.h"
#include "modules/gps/gps_provider.h"
#include "modules/gps/gps_web.h"
#include <WiFi.h>
#include <globals.h>
#include <algorithm>
#include <vector>

static bool require_casic() {
    if (bruceConfig.gpsSource != BruceConfig::GPS_SOURCE_CASIC) {
        serialDevice->println("GPS source is not CASIC. Set gps_source casic first.");
        return false;
    }
    return true;
}

static uint32_t gpsSourceCmd(cmd *c) {
    Command cmd(c);
    String val = cmd.getArg(0).getValue();
    val.toLowerCase();
    if (val == "casic") bruceConfig.setGpsSource(BruceConfig::GPS_SOURCE_CASIC);
    else bruceConfig.setGpsSource(BruceConfig::GPS_SOURCE_LEGACY);
    serialDevice->printf("gps source: %s\n", bruceConfig.gpsSource == BruceConfig::GPS_SOURCE_CASIC ? "casic" : "legacy");
    return true;
}

static uint32_t gpsBaudCmd(cmd *c) {
    Command cmd(c);
    int baud = cmd.getArg(0).getValue().toInt();
    bruceConfig.setGpsBaudrate(baud);
    serialDevice->printf("gps baud set to %d\n", bruceConfig.gpsBaudrate);
    if (require_casic()) {
        int applied = baud;
        if (gps_casic_set_baudrate(gps_casic_baud_code_from_value(baud), applied))
            serialDevice->printf("CASIC baud switched to %d\n", applied);
        else serialDevice->println("Failed to send PCAS01");
    }
    return true;
}

static uint32_t gpsRateCmd(cmd *c) {
    Command cmd(c);
    int rate = cmd.getArg(0).getValue().toInt();
    bruceConfig.setGpsUpdateRate(rate);
    serialDevice->printf("gps update rate set to %d ms\n", bruceConfig.gpsUpdateRateMs);
    if (require_casic()) {
        if (gps_casic_set_update_rate_ms(rate)) serialDevice->println("PCAS02 applied");
        else serialDevice->println("Invalid rate for PCAS02");
    }
    return true;
}

static uint32_t gpsSystemCmd(cmd *c) {
    if (!require_casic()) return false;
    Command cmd(c);
    uint8_t mode = cmd.getArg(0).getValue().toInt();
    if (gps_casic_set_system_mode(mode)) serialDevice->printf("System mode set to %u\n", mode);
    else serialDevice->println("Invalid system mode (1-7)");
    return true;
}

static uint32_t gpsNmeaCmd(cmd *c) {
    if (!require_casic()) return false;
    Command cmd(c);
    uint8_t ver = cmd.getArg(0).getValue().toInt();
    if (gps_casic_set_nmea_version(ver)) serialDevice->printf("NMEA version code %u applied\n", ver);
    else serialDevice->println("Failed to set NMEA version");
    return true;
}

static uint32_t gpsMuteAntCmd(cmd *c) {
    if (!require_casic()) return false;
    Command cmd(c);
    String val = cmd.countArgs() > 0 ? cmd.getArg(0).getValue() : "";
    val.toLowerCase();
    bool disableAntTxt = (val == "" || val == "on" || val == "1" || val == "true");
    gps_casic_nmea_cfg_t cfg{};
    cfg.gga = 1; cfg.gsa = 1; cfg.gsv = 1; cfg.rmc = 1; cfg.utc = 1; cfg.ant = disableAntTxt ? 0 : 1;
    if (gps_casic_configure_nmea(cfg)) serialDevice->printf("ANT TXT %s\n", disableAntTxt ? "disabled" : "enabled");
    else serialDevice->println("Failed to configure PCAS03");
    return true;
}

static uint32_t gpsResetCmd(cmd *c) {
    if (!require_casic()) return false;
    Command cmd(c);
    String val = cmd.getArg(0).getValue();
    val.toLowerCase();
    uint8_t mode = 0;
    if (val == "hot") mode = 0;
    else if (val == "warm") mode = 1;
    else if (val == "cold") mode = 2;
    else mode = 3;
    if (gps_casic_reset(mode)) serialDevice->printf("Reset sent (mode %u)\n", mode);
    else serialDevice->println("Reset failed");
    return true;
}

static uint32_t gpsSaveCmd(cmd *c) {
    if (!require_casic()) return false;
    if (gps_casic_save_config()) serialDevice->println("Config saved (PCAS00)");
    else serialDevice->println("Save failed");
    return true;
}

static uint32_t gpsInfoCmd(cmd *c) {
    if (!require_casic()) return false;
    gps_casic_info_t info{};
    if (gps_casic_query_info(&info, 800)) {
        serialDevice->println("CASIC info:");
        serialDevice->printf(" MA: %s\n IC: %s\n SW: %s\n Build: %s\n Mode: %s\n Customer: %s\n",
                      info.manufacturer, info.ic, info.sw, info.build_time, info.mode, info.customer_id);
    } else serialDevice->println("Failed to read info");
    return true;
}

static uint32_t gpsLogCmd(cmd *c) {
    Command cmd(c);
    bool enable = gps_casic_status_log_enabled();
    if (cmd.countArgs() > 0) {
        String val = cmd.getArg(0).getValue();
        val.toLowerCase();
        if (val == "on" || val == "1" || val == "true") enable = true;
        else if (val == "off" || val == "0" || val == "false") enable = false;
    }
    gps_casic_enable_status_log(enable);
    serialDevice->printf("gps log %s\n", enable ? "ON" : "OFF");
    return true;
}

static uint32_t gpsStatusOnceCmd(cmd *c) {
    (void)c;
    const gps_fix_t &f = gps_casic_get_fix();
    const gnss_sat_view_t &v = gps_casic_get_sat_view();
    serialDevice->printf("Fix: %s, mode %u, sats used %u, visible %u, HDOP %.1f, PDOP %.1f\n",
                  f.fix_valid ? "YES" : "NO", f.fix_type, f.sats_used, v.count, f.hdop, f.pdop);
    return true;
}

static String cn0_label(uint8_t cn0) {
    const char *qual = "very weak";
    if (cn0 >= 40) qual = "excellent";
    else if (cn0 >= 30) qual = "good";
    else if (cn0 >= 25) qual = "ok";
    else if (cn0 >= 20) qual = "weak";
    String out = String(cn0) + " dBHz (" + qual + ")";
    return out;
}

static uint32_t gpsSatsCmd(cmd *c) {
    if (!require_casic()) return false;
    (void)c;
    const gps_fix_t &f = gps_casic_get_fix();
    gnss_sat_view_t view = gps_casic_get_sat_view(); // copy

    std::vector<gnss_sat_t> sats(view.sats, view.sats + view.count);
    std::sort(sats.begin(), sats.end(), [](const gnss_sat_t &a, const gnss_sat_t &b) { return a.cn0_dbhz > b.cn0_dbhz; });

    serialDevice->println("=== GNSS summary ===");
    serialDevice->printf("Fix: %s (mode %u)\n", f.fix_valid ? "YES" : "NO", f.fix_type);
    serialDevice->printf("Position: lat %.6f lon %.6f alt %.1f m\n", f.lat_deg, f.lon_deg, f.alt_m);
    serialDevice->printf("Satellites: used %u / visible %u\n", f.sats_used, view.count);
    serialDevice->printf("Speed: %.1f km/h  Course: %.1f deg\n", f.speed_kph, f.course_deg);
    serialDevice->printf("Antenna: %d\n", gps_casic_get_antenna_status());
    serialDevice->println("--- Satellites (sorted by signal) ---");
    serialDevice->println("SYS SVID  C/N0  Signal     Elev Az   Used");
    for (const auto &s : sats) {
        const char *sys = "";
        switch (s.system) {
        case GNSS_SYS_GPS: sys = "GPS"; break;
        case GNSS_SYS_BDS: sys = "BDS"; break;
        case GNSS_SYS_GLONASS: sys = "GLN"; break;
        case GNSS_SYS_GALILEO: sys = "GAL"; break;
        case GNSS_SYS_QZSS: sys = "QZ"; break;
        case GNSS_SYS_SBAS: sys = "SB"; break;
        default: sys = "UNK"; break;
        }
        String cn0txt = cn0_label(s.cn0_dbhz);
        serialDevice->printf("%-3s %3u  %-16s %3u° %3u°   %s\n",
                      sys,
                      s.svid,
                      cn0txt.c_str(),
                      s.elevation_deg,
                      s.azimuth_deg,
                      s.used_in_fix ? "YES" : " no");
    }
    return true;
}

static uint32_t gpsSatAppCmd(cmd *c) {
    (void)c;
    serialDevice->println("gps_satapp: запуск через сериал небезопасен (TFT падает).");
    serialDevice->println("Используй либо меню на устройстве, либо gps_web (8081) или gps_sats в консоли.");
    return true;
}

static uint32_t gpsWebCmd(cmd *c) {
    Command cmd(c);
    String val = cmd.countArgs() ? cmd.getArg(0).getValue() : "status";
    val.toLowerCase();
    if (val == "on" || val == "1") {
        if (gps_web_start()) {
            String url = gps_web_url();
            serialDevice->printf("gps web: started at %s\n", url.length() ? url.c_str() : "unknown IP");
            if (!WiFi.isConnected() && (WiFi.getMode() & WIFI_MODE_AP)) {
                serialDevice->printf("Connect to device AP, then open %s\n", url.c_str());
            }
        } else serialDevice->println("gps web: failed (wifi not connected?)");
    } else if (val == "off" || val == "0") {
        gps_web_stop();
        serialDevice->println("gps web: stopped");
    } else {
        if (gps_web_running()) {
            String url = gps_web_url();
            serialDevice->printf("gps web: running at %s\n", url.length() ? url.c_str() : "unknown IP");
        } else {
            serialDevice->println("gps web: stopped");
        }
    }
    return true;
}

void createGpsCommands(SimpleCLI *cli) {
    Command src = cli->addCommand("gps_source", gpsSourceCmd);
    src.addPosArg("legacy|casic");

    Command baud = cli->addCommand("gps_baud", gpsBaudCmd);
    baud.addPosArg("baud");

    Command rate = cli->addCommand("gps_rate", gpsRateCmd);
    rate.addPosArg("ms");

    Command sys = cli->addCommand("gps_system", gpsSystemCmd);
    sys.addPosArg("mode1-7");

    Command nmea = cli->addCommand("gps_nmea", gpsNmeaCmd);
    nmea.addPosArg("ver");

    Command mute = cli->addCommand("gps_muteant", gpsMuteAntCmd);
    mute.addPosArg("on|off");

    Command rst = cli->addCommand("gps_reset", gpsResetCmd);
    rst.addPosArg("hot|warm|cold|factory");

    cli->addCommand("gps_save", gpsSaveCmd);
    cli->addCommand("gps_info", gpsInfoCmd);

    Command logcmd = cli->addCommand("gps_log", gpsLogCmd);
    logcmd.addPosArg("on|off");

    cli->addCommand("gps_status", gpsStatusOnceCmd);
    cli->addCommand("gps_sats", gpsSatsCmd);
    cli->addCommand("gps_satapp", gpsSatAppCmd);
    Command web = cli->addCommand("gps_web", gpsWebCmd);
    web.addPosArg("on|off");
}
