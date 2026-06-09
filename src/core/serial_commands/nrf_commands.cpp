#include "nrf_commands.h"
#include "helpers.h"
#include "modules/NRF24/nrf_hijack.h"
#include "modules/NRF24/nrf_sniffer.h"
#include <globals.h>

// nrf scan [time_ms=3000]
static uint32_t nrfScanCallback(cmd *c) {
    Command cmd(c);
    Argument timeArg = cmd.getArgument(0);
    uint32_t scanMs = timeArg.isSet() ? timeArg.getValue().toInt() : 3000;

    auto devices = nrf_sniffer_collect(scanMs, 25);
    serialDevice->printf("[NRF] found %u devices\n", (unsigned)devices.size());
    for (size_t i = 0; i < devices.size(); ++i) {
        auto &d = devices[i];
        serialDevice->printf(
            "%02u CH%3u %02X%02X%02X%02X%02X hits=%lu\n",
            (unsigned)(i + 1),
            d.channel,
            d.addr[0],
            d.addr[1],
            d.addr[2],
            d.addr[3],
            d.addr[4],
            (unsigned long)d.hits
        );
    }
    return true;
}

// nrf jam_sweep start stop step dwell noise(0/1)
static uint32_t nrfJamSweepCallback(cmd *c) {
    Command cmd(c);
    Argument a1 = cmd.getArgument(0);
    Argument a2 = cmd.getArgument(1);
    Argument a3 = cmd.getArgument(2);
    Argument a4 = cmd.getArgument(3);
    Argument a5 = cmd.getArgument(4);
    int startCh = a1.getValue().toInt();
    int stopCh = a2.getValue().toInt();
    int step = a3.getValue().toInt();
    int dwell = a4.getValue().toInt();
    bool noise = a5.getValue().toInt() != 0;

    serialDevice->printf(
        "[NRF] sweep jam start=%d stop=%d step=%d dwell=%d noise=%d\n", startCh, stopCh, step, dwell, noise
    );
    // Run sweep jam until user presses Esc on device; for serial we stop after one sweep
    unsigned long endAt = millis() + 3000;
    nrf_sweep_jam(startCh, stopCh, step, dwell, noise);
    while (millis() < endAt) delay(10);
    return true;
}

// nrf hijack <addr> <channel> <action> [arg] [proto]
//   action: type|run|calc|cmd|jam   proto: logi(default)|hid
static uint32_t nrfHijackCallback(cmd *c) {
    Command cmd(c);
    String addr = cmd.getArgument(0).getValue();
    int channel = cmd.getArgument(1).getValue().toInt();
    String action = cmd.getArgument(2).getValue();
    String arg = cmd.getArgument(3).getValue();
    String proto = cmd.getArgument(4).getValue();
    bool logitech = !(proto == "hid" || proto == "generic");
    return nrf_hijack_inject(addr, channel, action, arg, logitech) ? true : false;
}

// nrf readkeys <addr> <channel> [secs]
static uint32_t nrfReadKeysCallback(cmd *c) {
    Command cmd(c);
    String addr = cmd.getArgument(0).getValue();
    int channel = cmd.getArgument(1).getValue().toInt();
    int secs = cmd.getArgument(2).getValue().toInt();
    return nrf_readkeys(addr, channel, secs) ? true : false;
}

void createNrfCommands(SimpleCLI *cli) {
    Command nrf = cli->addCompositeCmd("nrf");
    Command scan = nrf.addCommand("scan", nrfScanCallback);
    scan.addPosArg("time", "3000");

    Command jamSweep = nrf.addCommand("jam_sweep", nrfJamSweepCallback);
    jamSweep.addPosArg("start", "1");
    jamSweep.addPosArg("stop", "80");
    jamSweep.addPosArg("step", "2");
    jamSweep.addPosArg("dwell", "60");
    jamSweep.addPosArg("noise", "0");

    Command hijack = nrf.addCommand("hijack", nrfHijackCallback);
    hijack.addPosArg("addr", "0000000000");
    hijack.addPosArg("channel", "1");
    hijack.addPosArg("action", "jam");
    hijack.addPosArg("arg", "");
    hijack.addPosArg("proto", "logi");

    Command readkeys = nrf.addCommand("readkeys", nrfReadKeysCallback);
    readkeys.addPosArg("addr", "0000000000");
    readkeys.addPosArg("channel", "1");
    readkeys.addPosArg("secs", "10");
}
