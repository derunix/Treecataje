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
}
