#pragma once

#include <Arduino.h>

// Mouse/keyboard hijack against NRF24-based 2.4GHz HID dongles.
// Lets you pick a target address (sniffer or manual) and send basic HID reports.
void nrf_hijack();
// Combined scanner/hijack/jam toolkit entrypoint
void nrf_toolkit();

// Mousejack attack with pre-defined payloads (calc, cmd, rickroll, backdoor, custom)
void nrf_mousejack();

// Headless sweep jam (for serial command)
void nrf_sweep_jam(int startCh, int stopCh, int step, int dwellMs, bool noise);
