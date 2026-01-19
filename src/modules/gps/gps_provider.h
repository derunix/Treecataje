#pragma once

#include "gps_types.h"

bool gps_provider_begin();
void gps_provider_end();
void gps_provider_tick();

gps_source_t gps_provider_current_source();
bool gps_provider_fix_updated(bool clear = true);
const gps_fix_t &gps_provider_get_fix();
const gnss_sat_view_t &gps_provider_get_sat_view();
gps_ant_status_t gps_provider_get_antenna_status();
bool gps_provider_has_fix();
bool gps_provider_seen_bytes(bool clear = false);
