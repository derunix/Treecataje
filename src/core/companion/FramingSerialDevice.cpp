#include "FramingSerialDevice.h"

void FramingSerialDevice::emitRsp(const String &line) {
    if (!_inner) return;
    _inner->println("RSP " + String(_reqId) + " " + line);
}

void FramingSerialDevice::out(const String &s, bool eol) {
    _lineBuf += s;
    int nl;
    while ((nl = _lineBuf.indexOf('\n')) >= 0) {
        String line = _lineBuf.substring(0, nl);
        line.replace("\r", "");
        emitRsp(line);
        _lineBuf = _lineBuf.substring(nl + 1);
    }
    if (eol) {
        emitRsp(_lineBuf);
        _lineBuf = "";
    }
}

void FramingSerialDevice::endRequest(int code) {
    if (_lineBuf.length()) {
        emitRsp(_lineBuf);
        _lineBuf = "";
    }
    if (_inner) _inner->println("END " + String(_reqId) + " " + String(code));
}

size_t FramingSerialDevice::println(const String &s) {
    out(s, true);
    return s.length();
}
size_t FramingSerialDevice::println(size_t n) {
    out(String((unsigned)n), true);
    return 1;
}
size_t FramingSerialDevice::println(uint32_t n) {
    out(String(n), true);
    return 1;
}
size_t FramingSerialDevice::println() {
    out("", true);
    return 0;
}
size_t FramingSerialDevice::println(int n, int format) {
    out(String(n, format), true);
    return 1;
}
size_t FramingSerialDevice::print(int n, int format) {
    out(String(n, format), false);
    return 1;
}
size_t FramingSerialDevice::print(const String &s) {
    out(s, false);
    return s.length();
}
void FramingSerialDevice::vprintf(const char *fmt, va_list args) {
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n < 0) return;
    out(String(buf), false);
}
size_t FramingSerialDevice::write(uint8_t *str, size_t size) {
    String s;
    s.reserve(size);
    for (size_t i = 0; i < size; i++) s += (char)str[i];
    out(s, false);
    return size;
}
