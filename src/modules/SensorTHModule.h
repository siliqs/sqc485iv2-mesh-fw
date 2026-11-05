/*
 * SensorTHModule.h — Siliqs SQS-TH-I built-in temp/humidity sensor over mesh.
 *
 * The SQS-Sensor-line counterpart of ModbusModule: instead of polling an external
 * RS485 device, it periodically reads the on-board TI HDC1080 (I2C 0x40) and sends
 * a fixed, versioned binary payload on its own private PortNum. The HDC1080 read +
 * condensation-heater logic is the validated driver from
 *   14_SQC485Iv2_basic/hardware_basic_test/sensor_test/src/sqs1080/main.cpp
 * (real-hardware PASS 2026-03-21), grafted into the Meshtastic SinglePortModule +
 * OSThread shape (same as ModbusModule / the official EnvironmentTelemetry module).
 *
 * Build guard: compiled only when -D SQS_TH_I is set (the board variant), so a
 * stock / SQC485Iv2 build is unaffected.
 *
 * Wire format (little-endian, 11 bytes) — this is a shipped CONTRACT; bump `ver`
 * before changing field meaning:
 *   [0] ver   u8   payload format version (=1)
 *   [1] model u8   product id (SQS-TH-I = 1) — one decoder for the whole SQS line
 *   [2] temp  i16  temperature  x100 (deg C); NAN/unread sent as 0 with flag bit0
 *   [4] rh    u16  relative humidity x100 (%RH)
 *   [6] vbat  u16  battery millivolts (0 = not measured yet / no battery)  [M4]
 *   [8] soc   u8   state of charge % (0..100); 0xFF = unknown / no battery  [M4]
 *   [9] flags u8   bit0 sensor NACK · bit1 heater ran · bit2 on USB · bit3 charging
 *                  · bit4 no battery                                        [M4 bits]
 *  [10] boot  u8   reboot/wake counter (wraps) — spot a node that keeps resetting
 */
#pragma once

#include "concurrency/OSThread.h"
#include "mesh/SinglePortModule.h"

// Private application PortNum for Siliqs SENSOR payloads. Distinct from the
// SQC485Iv2 Modbus raw-forward (256) so a gateway/decoder can tell the two apart
// unambiguously.
//
// Must stay in Meshtastic's private range (256–511) but AVOID the named entries:
// 256 = PRIVATE_APP, 257 = ATAK_FORWARDER (an official service — the meshtastic lib
// would deliver it to receivers as the string "ATAK_FORWARDER", and ATAK nodes would
// mis-handle our packets). 258 is unnamed, so it arrives as a plain int on the RX
// side and collides with nothing. (256 is also taken by our own Modbus forward, 260
// by the bridge's serial-over-MQTT.) Fixed value — the gateway/cloud decoder matches it.
#define SILIQS_SENSOR_PORTNUM ((meshtastic_PortNum)258)

#define SQS_TH_PAYLOAD_VER 1
#define SQS_MODEL_SQS_TH_I 1
#define SQS_TH_PAYLOAD_LEN 11

// Report period (seconds) — the FALLBACK default. The live cadence comes from the
// provisioned config (power.uplink_interval_s), so the Configurator can set it in the
// field; this value is used only when unset / implausibly small.
#ifndef SQS_TH_REPORT_INTERVAL_S
#define SQS_TH_REPORT_INTERVAL_S 60
#endif

class SensorTHModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    SensorTHModule();

  protected:
    // Periodic: read HDC1080 → build payload → mesh-send. Returns ms until next call.
    virtual int32_t runOnce() override;

    // Accept a config blob addressed to our PortNum so the Configurator can set the
    // report interval (and tx target) over USB/BLE/mesh — same 'SQ'+blob the SQC485Iv2
    // uses, validated by config_from_blob(). (SQS ignores the blob's Modbus/poll/tunnel
    // fields; only power.* and tx.* are meaningful here.)
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override
    {
        return p->decoded.portnum == ourPortNum;
    }

  private:
    bool firstTime = true;
    bool sensorOk = false;

    bool initSensor();                              // identity check + CONFIG (14-bit, cont.)
    bool readTempHumidity(float &tempC, float &rh); // trigger + read 4 bytes
    void runHeater();                               // >95%RH de-condensation cycle
    void takeReadingAndSend();                      // read (+heater) then send
    void sendEnvironmentMetrics(float tempC, float rh); // standard Meshtastic telemetry on port 67
    void buildAndSend(float tempC, float rh, bool heaterUsed, bool ok); // Siliqs health on port 258

    int32_t reportIntervalMs();                     // from provisioned config, clamped
    void applyConfigBlob(const uint8_t *blob, size_t len, uint32_t from);
    void sendReply(const uint8_t *data, size_t len, uint32_t to);

    // ── deep-sleep duty cycle (M4) — power-save leaf: read, send, sleep the radio +
    //    MCU for the interval, reboot on wake. Only for a CLIENT_MUTE leaf with
    //    deep_sleep configured (a relay/router must stay awake). ──────────────────
    int  sleepMode();          // 0 = stay awake, 4 = broadcast fire-and-forget sleep
    void enterDeepSleep();     // radio + esp_deep_sleep for the interval (no return)
    bool clientConnected();    // a configurator is attached over USB/BLE (defer sleep)
    bool     sleepArmed = false;       // reading sent last cycle; waiting for TX to drain
    uint32_t sleepArmedAt = 0;
    bool     bootChecked = false;      // one-shot: set the post-reset connect grace
    uint32_t connectGraceUntil = 0;    // stay awake until here after a reset (not a timer wake)
};

extern SensorTHModule *sensorTHModule;
