#pragma once

#include "gps_types.h"
#include <Arduino.h>
#include <vector>

class BruceConfigPins;
class HardwareSerial;

// Initialization / lifecycle
// UPDATED: Now accepts shared HardwareSerial pointer to avoid UART conflicts
bool gps_casic_init(int baudrate, const BruceConfigPins &pins, HardwareSerial **sharedSerial = nullptr);
void gps_casic_deinit();
void gps_casic_tick();

// State access
bool gps_casic_has_fix();
bool gps_casic_fix_updated(bool clear = true);
const gps_fix_t &gps_casic_get_fix();
const gnss_sat_view_t &gps_casic_get_sat_view();
gps_casic_state_t gps_casic_get_state();
gps_ant_status_t gps_casic_get_antenna_status();
bool gps_casic_seen_bytes(bool clear = false);

// Helpers
uint8_t nmea_checksum(const char *data, size_t len);
uint8_t gps_casic_baud_code_from_value(int baud);
int gps_casic_baud_from_code(uint8_t code);

// PCAS control commands
bool gps_casic_set_baudrate(uint8_t code, int &appliedBaud);
bool gps_casic_set_update_rate_ms(uint16_t ms);
bool gps_casic_configure_nmea(const gps_casic_nmea_cfg_t &cfg);
bool gps_casic_set_system_mode(uint8_t mode);
bool gps_casic_set_nmea_version(uint8_t ver);
bool gps_casic_query_info(gps_casic_info_t *out, uint32_t timeoutMs = 800);
bool gps_casic_reset(uint8_t mode);
bool gps_casic_standby(uint16_t seconds);
bool gps_casic_set_sv_mask(uint8_t sys_id, uint32_t mask_low, uint32_t mask_high = 0);
bool gps_casic_save_config();

// Logging control
void gps_casic_enable_status_log(bool enable);
bool gps_casic_status_log_enabled();
void gps_casic_get_recent_nmea(std::vector<String> &out, size_t maxLines = 120);
