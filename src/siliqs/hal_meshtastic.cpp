/*
 * hal_meshtastic.cpp — Meshtastic/Arduino implementation of the firmware_core HAL.
 *
 * This is the SECOND HAL port (the first being firmware_core/stub/* on host).
 * It lets the SAME portable engine (modbus.c / poll.c / codec.c / config.c) run
 * inside the Meshtastic firmware unchanged — proving the HAL boundary.
 *
 * SKELETON: compiles within a Meshtastic/Arduino build (Serial1, LittleFS exist
 * there), not standalone. Pin numbers come from the v233 board profile.
 * Implements only the HAL functions the mesh module actually uses (serial, time,
 * store, power, led); radio/link are owned by Meshtastic, not the engine.
 */
#ifdef SQC485IV2  // only build this HAL for the SQC485I board envs

#include <Arduino.h>
#include <LittleFS.h>

extern "C" {
#include "hal/hal_serial.h"
#include "hal/hal_time.h"
#include "hal/hal_store.h"
#include "hal/hal_power.h"
#include "board_profile.h"
}

// DE polarity: v233 inverts GPIO9 -> DE via U7 (default), v231 drives DE directly
// from GPIO9 (the env passes -D SQC485I_DE_INVERTED=0). Mirrors PLCModbusModule.
#ifndef SQC485I_DE_INVERTED
#define SQC485I_DE_INVERTED 1
#endif

// ── Board profile. Same data as firmware_core/src/board_v233.c, but DE polarity
//    follows the build flag so one HAL serves both v233 and v231 envs. ──────────
extern "C" const board_profile_t BOARD = {
    .name = "SQC485I",
    .rs485_de_inverted = (bool)SQC485I_DE_INVERTED,
    .rs485_tx_echo = true,
    .rs485_power_switched = false,
    .has_ble = true,
};

#define RS485_RX_PIN 0
#define RS485_TX_PIN 1
#define RS485_DE_PIN 9
// v233: receive-enable #RE is on GPIO2 (active-low) and is NOT tied low on the
// board — firmware MUST drive it LOW or the receiver stays off (symptom: bus rx 0
// even though the request goes out and the slave replies). The pin is shared with
// the white LED, so the LED is sacrificed to keep the receiver enabled.
// See reference_sqc485i_v233_rs485_pinout / docs/HW_BRINGUP_LOG.md.
#define RS485_RE_PIN 2

// GPIO2 doubles as the (active-LOW) status LED and the RS485 #RE line. Held LOW keeps the receiver
// enabled AND lights the LED, so we keep it LOW whether polling or idle → the LED is a simple steady
// "on = alive" indicator on this headless board.

// ── hal_serial → Serial1 (RS485), DE on GPIO with board polarity ───────────────
extern "C" bool hal_serial_init(uint32_t baud, hal_parity_t parity, uint8_t stop_bits)
{
    (void)parity; (void)stop_bits;  // extend with SERIAL_8E1 etc. as needed
    pinMode(RS485_DE_PIN, OUTPUT);
    digitalWrite(RS485_DE_PIN, BOARD.rs485_de_inverted ? HIGH : LOW); // idle = RX
    pinMode(RS485_RE_PIN, OUTPUT);
    digitalWrite(RS485_RE_PIN, LOW);   // #RE held LOW = receiver enabled (LED on)
    Serial1.begin(baud, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
    return true;
}

// Called while RS485 polling is OFF: keep the status LED on (GPIO2 held LOW) so an idle node
// (gateway/collector, or RS485 disabled) still shows a lit LED instead of going dark.
extern "C" void hal_led_idle_on(void)
{
    static bool inited = false;
    if (!inited) { pinMode(RS485_RE_PIN, OUTPUT); inited = true; }
    digitalWrite(RS485_RE_PIN, LOW);
}

extern "C" void hal_serial_set_tx(bool enable)
{
    bool level = BOARD.rs485_de_inverted ? !enable : enable;
    digitalWrite(RS485_DE_PIN, level ? HIGH : LOW);
}

extern "C" int hal_serial_write(const uint8_t *buf, size_t len) { return (int)Serial1.write(buf, len); }
extern "C" void hal_serial_flush(void) { Serial1.flush(); }

extern "C" int hal_serial_read(uint8_t *buf, size_t len, uint32_t to_ms)
{
    uint32_t start = millis();
    size_t got = 0;
    while (got < len && (millis() - start) < to_ms)
        if (Serial1.available()) buf[got++] = (uint8_t)Serial1.read();
    return (int)got;
}

// ── hal_time → Arduino ─────────────────────────────────────────────────────────
extern "C" uint32_t hal_millis(void) { return millis(); }
extern "C" void hal_delay(uint32_t ms) { delay(ms); }
extern "C" void hal_deep_sleep(uint32_t ms) { (void)ms; } // Meshtastic owns sleep policy
extern "C" void hal_watchdog_set(uint32_t ms) { (void)ms; }
extern "C" void hal_watchdog_feed(void) {}

// ── hal_store → LittleFS file (our own config blob; NOT Meshtastic ModuleConfig) ─
#define CFG_PATH "/siliqs_modbus.cfg"

extern "C" bool hal_store_init(void) { return LittleFS.begin(); }

extern "C" int hal_store_get(const char *key, void *out, size_t max_len)
{
    (void)key;  // single-blob store; key reserved for future multi-key use
    File f = LittleFS.open(CFG_PATH, "r");
    if (!f) return -1;
    int n = (int)f.read((uint8_t *)out, max_len);
    f.close();
    return n;
}

extern "C" bool hal_store_set(const char *key, const void *data, size_t len)
{
    (void)key;
    File f = LittleFS.open(CFG_PATH, "w");
    if (!f) return false;
    size_t n = f.write((const uint8_t *)data, len);
    f.close();
    return n == len;
}

extern "C" bool hal_store_erase(const char *key) { (void)key; return LittleFS.remove(CFG_PATH); }
extern "C" bool hal_store_commit(void) { return true; }  // LittleFS writes are durable

// ── hal_power / led ────────────────────────────────────────────────────────────
extern "C" uint16_t hal_supply_mv(void) { return 0; } // TODO: ADC on supply divider
// No-op: the only LED on v233 is on GPIO2, which we hold LOW as RS485 #RE (see
// above). Toggling it here would disable the receiver. Meshtastic drives its own
// status LED elsewhere; the engine's LED calls are intentionally ignored.
extern "C" void hal_led_set(bool on) { (void)on; }

#endif // SQC485IV2
