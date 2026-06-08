#include "serialcmds.h"
#include "companion/companion.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "utils.h"
#include <core/USBSerial/USBSerial.h>
#include <globals.h>

#if !defined(LITE_VERSION)
// Set while the BLE-API is up (modules/ble_api). We poll it alongside USB so a
// USB and a BLE companion session can run at the same time.
extern SerialDevice *bleApiSerial;
#endif

QueueHandle_t cmdQueue = nullptr;
QueueHandle_t rspQueue = nullptr;
TaskHandle_t serialcmdsTaskHandle;

struct CmdPacket {
    char text[512]; // command size
};
bool parseSerialCommand(const String &command, bool waitForResponse) {
    if (!cmdQueue || !rspQueue) {
        Serial.println("Command or response queue not initialized");
        return false;
    }
    CmdPacket packet;
    memset(&packet, 0, sizeof(packet));
    strncpy(packet.text, command.c_str(), sizeof(packet.text) - 1);

    // Enqueue the command packet for processing
    if (xQueueSend(cmdQueue, &packet, 0) != pdTRUE) {
        Serial.println("Failed to send command to queue");
        return false;
    }
    if (!waitForResponse) { return true; }
    // Wait for the response
    bool result;
    if (xQueueReceive(rspQueue, &result, pdMS_TO_TICKS(20))) { return result; }
    Serial.println("Failed to receive command response");
    return false;
}

// Process one waiting line from a single transport. Companion frames are handled
// non-modally (no backToMenu) with replies routed back to THIS transport; plain
// CLI lines run the legacy path with output routed here too.
static void pumpTransport(SerialCli &serialCli, SerialDevice *dev) {
    if (!dev || !dev->available()) return;
    String cmd_str = dev->readStringUntil('\n');

    SerialDevice *saved = serialDevice;
    serialDevice = dev; // route default/legacy output to this transport

    if (bruceConfig.companionEnabled && companion::looksLikeFrame(cmd_str)) {
        // framed, non-modal — replies go back to `dev`, screen untouched
        companion::handleLine(serialCli, cmd_str, dev);
    } else {
        Serial.println("COMMAND: " + cmd_str);
        serialCli.parse(cmd_str);
        dev->print("# ");        // prompt
        backToMenu();            // forced menu redraw (legacy behavior)
    }

    serialDevice = saved;
}

void handleSerialCommands(SerialCli &serialCli) {
    CmdPacket packet;
    if (cmdQueue && rspQueue) {
        if (xQueueReceive(cmdQueue, &packet, 0) == pdTRUE) {
            bool result = serialCli.parse(String(packet.text));
            xQueueSend(rspQueue, &result, 0);
            Serial.println("COMMAND: " + String(packet.text));
            Serial.printf("[CLI] Result: %s\n", result ? "TRUE" : "FALSE");
        }
    }
    // Drain any active companion stream (emits async EVT frames). Non-blocking.
    if (bruceConfig.companionEnabled) companion::tick();

    // Poll USB always, and BLE when the BLE-API is up — both run concurrently.
    pumpTransport(serialCli, &USBserial);
#if !defined(LITE_VERSION)
    if (bleApiSerial) pumpTransport(serialCli, bleApiSerial);
#endif
}

void _serialCmdsTaskLoop(void *pvParameters) {
    Serial.begin(115200);
    while (1) {
        handleSerialCommands(serialCli);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void startSerialCommandsHandlerTask(bool initQueues) {
    if (initQueues) {
        cmdQueue = xQueueCreate(2, sizeof(CmdPacket));
        rspQueue = xQueueCreate(2, sizeof(bool));
    }

    xTaskCreatePinnedToCore(
        _serialCmdsTaskLoop,         // Function to implement the task
        "serialcmds",                // Name of the task (any string)
        SERIAL_CMDS_TASK_STACK_SIZE, // Stack size in bytes
        NULL, // This is a pointer to the parameter that will be passed to the new task. We are not using it
              // here and therefore it is set to NULL.
        2,    // Priority of the task
        &serialcmdsTaskHandle, // Task handle (optional, can be NULL).
#if SOC_CPU_CORES_NUM > 1
        1 // Core where the task should run. By default, all your Arduino code runs on Core 1 and the Wi-Fi
          // and RF functions
#else
        0 // Core where the task should run. ESP32-C5 has only one core
#endif
    ); // (these are usually hidden from the Arduino environment) use the Core 0.
    if (!serialcmdsTaskHandle) { Serial.println("Failed to create Serial Commands Handler task"); }
}
