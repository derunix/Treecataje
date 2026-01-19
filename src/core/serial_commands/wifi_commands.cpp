#include "wifi_commands.h"
#include "core/wifi/webInterface.h"
#include "core/wifi/wifi_common.h" //to return MAC addr
#include <globals.h>

uint32_t wifiCallback(cmd *c) {
    Command cmd(c);
    Argument statusArg = cmd.getArgument("status");
    String status = statusArg.getValue();
    status.trim();

    Argument ssidArg = cmd.getArgument("ssid");
    String ssid = ssidArg.getValue();
    ssid.trim();

    Argument pwdArg = cmd.getArgument("pwd");
    String pwd = pwdArg.getValue();
    pwd.trim();

    if (status.length() == 0 || status == "status") {
        Serial.printf(
            "WiFi: %s\nIP: %s\nMode: %s\n",
            wifiConnected ? "connected" : "disconnected",
            wifiConnected ? wifiIP.c_str() : "-",
            WiFi.getMode() == WIFI_AP ? "AP" : (WiFi.getMode() == WIFI_STA ? "STA" : "AP+STA")
        );
        return true;
    } else if (status == "off") {
        wifiDisconnect();
        return true;
    } else if (status == "on") {
        if (wifiConnected) {
            Serial.println("Wifi already connected");
            return true;
        }
        if (wifiConnecttoKnownNet()) return true;
        wifiDisconnect();
        return _setupAP();

    } else if (status == "add" && ssid != "" && pwd != "") {
        bruceConfig.addWifiCredential(ssid, pwd);
        return true;
    } else {
        Serial.println(
            "Invalid status: " + status + "\n"
            "Possible commands:\n"
            "-> wifi             (show status)\n"
            "-> wifi on          (connect to known WiFi or start AP)\n"
            "-> wifi off         (disconnect WiFi)\n"
            "-> wifi add SSID PASSWORD (store credentials)"
        );
        return false;
    }
}

uint32_t webuiCallback(cmd *c) {
    Command cmd(c);

    Argument arg = cmd.getArgument("noAp");
    bool noAp = arg.isSet();

    Serial.println("Starting Web UI " + !noAp ? "AP" : "STA");
    Serial.println("Press ESC to quit");
    startWebUi(!noAp); // MEMO: will quit when check(EscPress)

    return true;
}

void createWifiCommands(SimpleCLI *cli) {
    Command webuiCmd = cli->addCommand("webui", webuiCallback);
    webuiCmd.addFlagArg("noAp");

    Command wifiCmd = cli->addCommand("wifi", wifiCallback);
    wifiCmd.addPosArg("status", "");
    wifiCmd.addPosArg("ssid", "");
    wifiCmd.addPosArg("pwd", "");
}
