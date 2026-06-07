#pragma once
// FramingSerialDevice — a SerialDevice decorator that wraps every line of
// output from an existing command into a companion-protocol RSP frame, then
// appends an END frame when the request finishes.
//
// It implements the same SerialDevice interface, so the 80+ existing CLI
// commands need ZERO changes: while a companion REQ is being processed we
// temporarily point the global `serialDevice` at this decorator (it forwards
// to the real underlying transport, USBSerial or BLESerialService).
//
// See docs/companion/firmware.md and docs/companion/protocol.md.
#include <Arduino.h>
#include <SerialDevice.h>

class FramingSerialDevice : public SerialDevice {
public:
    explicit FramingSerialDevice(SerialDevice *inner) : _inner(inner) {}

    void setInner(SerialDevice *inner) { _inner = inner; }
    void beginRequest(uint32_t id) {
        _reqId = id;
        _lineBuf = "";
    }
    // Flush any pending partial line, then emit "END <id> <code>".
    void endRequest(int code);

    // --- SerialDevice interface (output is wrapped as RSP frames) ---
    size_t println(const String &s) override;
    size_t println(size_t n) override;
    size_t println(uint32_t n) override;
    size_t println() override;
    size_t println(int n, int format) override;
    size_t print(int n, int format = DEC) override;
    size_t print(const String &s) override;
    void vprintf(const char *fmt, va_list args) override;
    size_t write(uint8_t *str, size_t size) override;
    void flush() override {}

    // Input is proxied straight through to the real transport.
    int available() override { return _inner ? _inner->available() : 0; }
    String readStringUntil(char terminator) override {
        return _inner ? _inner->readStringUntil(terminator) : String("");
    }

private:
    void emitRsp(const String &line);
    // Append text; emit a RSP frame for every complete line; if eol, flush rest.
    void out(const String &s, bool eol);

    SerialDevice *_inner;
    uint32_t _reqId = 0;
    String _lineBuf;
};
