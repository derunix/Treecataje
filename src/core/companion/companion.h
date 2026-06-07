#pragma once
// Companion protocol handler — see docs/companion/protocol.md.
//
// Bridges the framed companion wire protocol to the existing SerialCli
// dispatcher WITHOUT calling backToMenu(), so the device's standalone UI is
// not disturbed (the core "non-modal coexistence" requirement).
#include <Arduino.h>

class SerialCli;

namespace companion {

// Cheap test: does this incoming line look like a companion frame?
// (Plain human CLI commands never start with these tokens, so legacy use is
// completely unaffected.)
bool looksLikeFrame(const String &line);

// Process exactly one companion frame line (one BLE write == one frame).
void handleLine(SerialCli &cli, const String &raw);

// Called every serial-task iteration: emits queued async EVT frames (streaming).
// Non-blocking; no-op unless a stream is active.
void tick();

// Drop the authenticated session (call when a BLE central disconnects, so the
// next connection must re-authenticate from scratch).
void resetAuth();

} // namespace companion
