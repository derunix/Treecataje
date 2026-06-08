#if !defined(LITE_VERSION)
#include "ble_api.hpp"
#include <NimBLEDevice.h>
#include <core/USBSerial/USBSerial.h>
#include <core/companion/companion.h>
#include <globals.h>

// Pointer to the BLE serial device while the BLE-API is up (nullptr otherwise).
// The serial loop polls this IN ADDITION to USB so both transports work at once;
// we no longer hijack the global `serialDevice` (it stays = USBserial).
SerialDevice *bleApiSerial = nullptr;

BLE_API::BLE_API() = default;

class BLEAPICallback : public NimBLEServerCallbacks {
    BLE_API *api;

    void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
        pServer->updateConnParams(connInfo.getConnHandle(), 6, 24, 0, 400); // Improve latency
    };

    void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override {
        // Forget the companion auth session so the next central re-authenticates.
        companion::resetAuth();
    };

    void onMTUChange(uint16_t MTU, NimBLEConnInfo &connInfo) override { api->update_mtu(MTU); };

public:
    explicit BLEAPICallback(BLE_API *api) : api(api) {}
};

void BLE_API::setup() {
    NimBLEDevice::init("Bruce");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // 9 dBm, tweak if you want
    NimBLEDevice::setMTU(517);              // request a large ATT MTU for fast file xfer

    pServer = NimBLEDevice::createServer();
    pServer->advertiseOnDisconnect(true);
    pServer->setCallbacks(new BLEAPICallback(this));

    battery_service.setup(pServer);
    serial_service.setup(pServer);
    bleApiSerial = &serial_service; // expose for the dual-transport serial loop

    BLEAdvertising *pAdvertising = pServer->getAdvertising();
    pAdvertising->enableScanResponse(false); // Save some battery
    pAdvertising->setName("Bruc");           // Bruce is too long for adv packet len
    pAdvertising->start();
}

void BLE_API::update_mtu(uint16_t mtu) {
    battery_service.setMTU(mtu);
    serial_service.setMTU(mtu);
}

void BLE_API::end() {
    battery_service.end();
    serial_service.end();
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    esp_bt_controller_deinit();
#else
    BLEDevice::deinit();
#endif
    bleApiSerial = nullptr;
}
#endif
