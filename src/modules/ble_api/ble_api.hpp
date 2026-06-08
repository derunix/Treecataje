#ifndef BLE_API_HPP
#define BLE_API_HPP
#if !defined(LITE_VERSION)
#include "services/BLESerialService.h"
#include "services/BatteryService.hpp"

class SerialDevice;
// BLE serial device while the BLE-API is up (nullptr otherwise). Polled by the
// serial loop alongside USB so USB and BLE companion sessions run concurrently.
extern SerialDevice *bleApiSerial;

class BLE_API {
public:
    BLE_API();
    void setup();
    void end();
    void update_mtu(uint16_t mtu);

private:
    NimBLEServer *pServer;
    BatteryService battery_service;
    BLESerialService serial_service;
};
#endif
#endif // BLE_API_HPP
