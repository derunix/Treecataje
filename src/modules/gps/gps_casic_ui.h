#pragma once

#include "gps_types.h"

class GPSCasicSatView {
public:
    GPSCasicSatView();
    ~GPSCasicSatView();

private:
    void setup();
    void loop();
};

class GPSCasicInfoScreen {
public:
    GPSCasicInfoScreen();
    ~GPSCasicInfoScreen();

private:
    void setup();
    void loop();
};

class GPSCasicConfigMenu {
public:
    GPSCasicConfigMenu();
    ~GPSCasicConfigMenu();

private:
    void setup();
    void loop();
    void apply_system_mode();
    void apply_baudrate();
    void apply_update_rate();
    void apply_nmea_profile();
    void apply_mute_ant_txt();
    void apply_save();
    void apply_reset();
    void apply_standby();
    void apply_sv_mask();
};
