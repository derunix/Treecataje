#include "status_commands.h"

#include "core/batteryLogger.h"
#include "core/systemStatus.h"
#include "modules/gps/gps_casic.h"
#include <globals.h>

uint32_t statusCallback(cmd *c) {
    (void)c;

    BatteryLogger::BatteryStatus battery = BatteryLogger::currentStatus();

    Serial.println("System status:");
    Serial.printf(
        "CPU: %lu MHz, load: %u%%\n",
        static_cast<unsigned long>(SystemStatus::cpuFrequencyMhz()),
        static_cast<unsigned>(SystemStatus::cpuLoadPercent())
    );
    Serial.printf("WiFi: %s\n", wifiConnected ? "connected" : "not connected");
    Serial.printf("BLE: %s\n", BLEConnected ? "connected" : "not connected");
    Serial.printf("SD card: %s\n", sdcardMounted ? "mounted" : "not mounted");
    Serial.printf("Battery: %d%%", battery.percent);
    if (battery.voltageValid) Serial.printf(" (%.3fV)", battery.voltage);
    Serial.printf(", charging: %s\n", battery.charging ? "yes" : "no");

    return true;
}

uint32_t gpsMonCallback(cmd *c) {
    Command cmd(c);
    bool enable = gps_casic_status_log_enabled();
    if (cmd.countArgs() > 0) {
        String arg = cmd.getArg(0).getValue();
        arg.toLowerCase();
        if (arg == "on" || arg == "1" || arg == "true") enable = true;
        else if (arg == "off" || arg == "0" || arg == "false") enable = false;
    }
    gps_casic_enable_status_log(enable);
    Serial.printf("gpsmon: logging %s\n", enable ? "ON" : "OFF");
    return true;
}

void createStatusCommands(SimpleCLI *cli) {
    cli->addCommand("status", statusCallback);
    Command cmd = cli->addCommand("gpsmon", gpsMonCallback);
    cmd.addArg("state");
}
