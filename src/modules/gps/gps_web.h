#pragma once
#include <Arduino.h>
#include <WiFi.h>

// Lightweight optional web dashboard for GPS/CASIC
// Start/stop are safe to call repeatedly.
bool gps_web_start(uint16_t port = 8081);
void gps_web_stop();
bool gps_web_running();
String gps_web_url();
