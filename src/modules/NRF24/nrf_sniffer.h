#pragma once

#include <Arduino.h>
#include <RF24.h>
#include <vector>

// Basic info for a sniffed NRF24 address.
struct NrfSniffedDevice {
    uint8_t addr[5];
    uint8_t channel;
    uint8_t pipe;
    uint8_t sample[32];
    uint8_t sampleLen;
    uint32_t hits;
    uint32_t firstSeen;
    uint32_t lastSeen;
    String vendor;
    String type;
};

// Passive sniffer for NRF24 Enhanced ShockBurst style traffic (UI mode).
// Collects observed 5-byte addresses and shows basic vendor/type guesses.
void nrf_sniffer();

// Interactive sniffer with device selection and per-device actions
// Allows monitoring, jamming, and hijacking specific devices
void nrf_sniffer_interactive();

// Headless collector: scan the band and return the list of seen addresses.
std::vector<NrfSniffedDevice> nrf_sniffer_collect(uint32_t scanTimeMs = 3000, uint16_t dwellPerChannelMs = 20);

// Packet analyzer with logging - captures packets, decodes HID actions, logs to SD card
void nrf_packet_analyzer();
