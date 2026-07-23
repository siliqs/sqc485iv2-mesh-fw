/*
 * NullStream.h — a Stream that goes nowhere: reads return empty, writes are dropped.
 *
 * Used by the SQ_USB_TUNNEL build to take the Meshtastic console OFF the USB CDC
 * (compile with -D USER_DEBUG_PORT=g_nullStream), so the USB port is free for the
 * raw USB↔RS485 data tunnel. The console object still exists (logs become no-ops,
 * no null-deref), it just writes to / reads from this sink instead of Serial.
 */
#pragma once
// Force-included into every TU by the SQ_USB_TUNNEL build, so guard for C++ only
// (the firmware_core .c files would choke on `class`).
#ifdef __cplusplus
#include <Arduino.h>   // Stream / Print

class NullStream : public Stream {
  public:
    int    available() override { return 0; }
    int    read() override { return -1; }
    int    peek() override { return -1; }
    size_t write(uint8_t) override { return 1; }
    size_t write(const uint8_t *, size_t size) override { return size; }
    void   flush() override {}
    // Mimic the bits of the HardwareSerial API the SerialConsole calls on its port.
    void   begin(unsigned long = 0) {}
    void   begin(unsigned long, uint32_t) {}
    operator bool() const { return true; }   // so `while (!Port)` exits immediately
};

extern NullStream g_nullStream;
#endif // __cplusplus
