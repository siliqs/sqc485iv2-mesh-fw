/*
 * SensorTHModule.cpp — SQS-TH-I HDC1080 temp/humidity → mesh (see SensorTHModule.h).
 *
 * HDC1080 driver logic ported verbatim from the validated bench test
 *   14_SQC485Iv2_basic/hardware_basic_test/sensor_test/src/sqs1080/main.cpp
 * (identity 0x5449/0x1050, CONFIG 0x1000 = temp+humidity 14-bit continuous, read =
 * point at 0x00 → wait 15 ms → read 4 bytes, heater 0x3000 for 1 s then 0x1000).
 */
#include "configuration.h"

#ifdef SQS_TH_I

#include "MeshService.h"
#include "NodeDB.h"            // config.device.role (leaf vs router gate for sleep)
#include "RadioLibInterface.h" // radio sleep + isSending() TX-drain before power-down
#include "SensorTHModule.h"
#include "gps/RTC.h"                                // getTime() for the telemetry timestamp
#include "main.h"
#include "mesh/generated/meshtastic/telemetry.pb.h" // meshtastic_Telemetry / EnvironmentMetrics
#include "mesh/mesh-pb-constants.h"                 // pb_encode_to_bytes
#include <Arduino.h>
#include <Wire.h>
#include <esp_sleep.h>

extern "C" {
#include "config.h" // sq_config_t, config_load/config_save/config_from_blob (firmware_core)
}

// I2C pins come from the board variant (variants/esp32c3/sqs-th-i/variant.h).
#ifndef I2C_SDA
#define I2C_SDA 20
#endif
#ifndef I2C_SCL
#define I2C_SCL 21
#endif

// ── HDC1080 registers ──────────────────────────────────────────────────────
#define HDC1080_ADDR 0x40
#define HDC1080_REG_TEMPERATURE 0x00
#define HDC1080_REG_CONFIG 0x02
#define HDC1080_REG_MANUFACTURER_ID 0xFE // expect 0x5449 ("TI")
#define HDC1080_REG_DEVICE_ID 0xFF       // expect 0x1050
#define HDC1080_CONFIG_ACQ_MODE 0x1000   // temp + humidity, 14-bit, heater off
#define HDC1080_CONFIG_HEAT_ON 0x3000    // heater on (bit13) + ACQ mode
#define HDC1080_CONFIG_HEAT_OFF 0x1000   // heater off + ACQ mode
#define HDC1080_HEATER_ON_MS 1000
#define HDC1080_HEATER_SETTLE_MS 200
#define HDC1080_HUMIDITY_HIGH_THRESHOLD 95.0f

// payload flag bits
#define SQS_FLAG_SENSOR_NACK 0x01
#define SQS_FLAG_HEATER_RAN 0x02
// bits 2..4 (on-USB / charging / no-battery) land in M4 with battery telemetry.

SensorTHModule *sensorTHModule;

// Provisioned config (report interval + tx target) — loaded from the firmware_core
// LittleFS store. Defaults give uplink_interval_s = 60 on an unprovisioned node.
static sq_config_t g_cfg;

// Deep-sleep timing (M4). MIN drain covers Meshtastic's CSMA/tx-delay so the broadcast
// actually starts before we cut power; MAX is a ceiling so a wedged radio can't block
// power-down. CONNECT_GRACE keeps a just-reset node awake long enough for a configurator
// to attach before the first sleep (a duty-cycle timer wake gets no grace).
static const uint32_t SQS_L4_MIN_DRAIN_MS = 1500;
static const uint32_t SQS_L4_MAX_DRAIN_MS = 8000;
static const uint32_t SQS_CONNECT_GRACE_MS = 30000;

// Survives deep sleep (M4 duty-cycle); a power-on reset zeroes it.
RTC_DATA_ATTR static uint16_t sqsBootCount = 0;

// ── low-level I2C helpers (from the validated driver) ──────────────────────
static void hdcWriteReg16(uint8_t reg, uint16_t val)
{
    Wire.beginTransmission(HDC1080_ADDR);
    Wire.write(reg);
    Wire.write((uint8_t)(val >> 8));
    Wire.write((uint8_t)(val & 0xFF));
    Wire.endTransmission();
}

static uint16_t hdcReadReg16(uint8_t reg)
{
    Wire.beginTransmission(HDC1080_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false); // repeated start
    Wire.requestFrom((uint8_t)HDC1080_ADDR, (uint8_t)2);
    if (Wire.available() < 2)
        return 0xFFFF;
    uint8_t msb = Wire.read();
    uint8_t lsb = Wire.read();
    return (uint16_t)((msb << 8) | lsb);
}

SensorTHModule::SensorTHModule()
    : SinglePortModule("sensorth", SILIQS_SENSOR_PORTNUM), concurrency::OSThread("SensorTH")
{
    config_load(&g_cfg); // defaults (uplink_interval_s = 60) if first boot / unprovisioned
}

// Report cadence comes from the provisioned config so the Configurator can set it in
// the field; clamp a too-small / unset value to the compile-time default.
int32_t SensorTHModule::reportIntervalMs()
{
    uint32_t s = g_cfg.power.uplink_interval_s;
    if (s < 5)
        s = SQS_TH_REPORT_INTERVAL_S;
    return (int32_t)s * 1000;
}

bool SensorTHModule::initSensor()
{
    uint16_t mfr = hdcReadReg16(HDC1080_REG_MANUFACTURER_ID);
    uint16_t dev = hdcReadReg16(HDC1080_REG_DEVICE_ID);
    if (mfr != 0x5449 || dev != 0x1050) {
        LOG_WARN("SensorTH: HDC1080 not found (MFR=0x%04X DEV=0x%04X) — check I2C %d/%d", mfr, dev,
                 (int)I2C_SDA, (int)I2C_SCL);
        return false;
    }
    hdcWriteReg16(HDC1080_REG_CONFIG, HDC1080_CONFIG_ACQ_MODE);
    delay(15); // power-on reset settle
    LOG_INFO("SensorTH: HDC1080 ready (MFR=0x%04X DEV=0x%04X)", mfr, dev);
    return true;
}

void SensorTHModule::runHeater()
{
    hdcWriteReg16(HDC1080_REG_CONFIG, HDC1080_CONFIG_HEAT_ON);
    delay(HDC1080_HEATER_ON_MS);
    hdcWriteReg16(HDC1080_REG_CONFIG, HDC1080_CONFIG_HEAT_OFF);
    delay(HDC1080_HEATER_SETTLE_MS);
}

bool SensorTHModule::readTempHumidity(float &tempC, float &rh)
{
    // Trigger: point at TEMPERATURE; the HDC1080 auto-measures temp then humidity.
    Wire.beginTransmission(HDC1080_ADDR);
    Wire.write(HDC1080_REG_TEMPERATURE);
    if (Wire.endTransmission() != 0)
        return false;
    delay(15); // 14-bit temp (6.35 ms) + 14-bit humidity (6.35 ms)
    Wire.requestFrom((uint8_t)HDC1080_ADDR, (uint8_t)4);
    if (Wire.available() < 4)
        return false;
    uint16_t rawTemp = ((uint16_t)Wire.read() << 8) | Wire.read();
    uint16_t rawHumi = ((uint16_t)Wire.read() << 8) | Wire.read();
    tempC = (rawTemp / 65536.0f) * 165.0f - 40.0f;
    rh = (rawHumi / 65536.0f) * 100.0f;
    return true;
}

void SensorTHModule::takeReadingAndSend()
{
    float tempC = 0.0f, rh = 0.0f;
    bool heaterUsed = false;
    bool ok = false;

    if (!sensorOk)
        sensorOk = initSensor(); // retry a sensor that wasn't ready at boot
    if (sensorOk) {
        ok = readTempHumidity(tempC, rh);
        if (ok && rh >= HDC1080_HUMIDITY_HIGH_THRESHOLD) {
            runHeater();
            ok = readTempHumidity(tempC, rh);
            heaterUsed = true;
        }
        if (!ok)
            sensorOk = false; // force re-identify next cycle
    }
    // Primary, ecosystem-standard reading: a Meshtastic EnvironmentMetrics telemetry on
    // TELEMETRY_APP (67) — decoded natively by any Meshtastic app / MQTT integration. Only
    // when the read succeeded (a NACK has no real value to report; the health packet flags it).
    if (ok)
        sendEnvironmentMetrics(tempC, rh);
    // Siliqs-specific health/diagnostics (heater/NACK/model/boot) on our private PortNum 258 —
    // fields standard telemetry has no place for. Small; consumed by the Siliqs gateway.
    buildAndSend(tempC, rh, heaterUsed, ok);
}

// Standard Meshtastic EnvironmentMetrics on TELEMETRY_APP (67), encoded from our own module
// (no upstream HDC1080 driver / no I2C autoscan needed — we read the sensor ourselves and
// just emit the standard protobuf so the whole Meshtastic ecosystem understands it).
void SensorTHModule::sendEnvironmentMetrics(float tempC, float rh)
{
    meshtastic_Telemetry t = meshtastic_Telemetry_init_zero;
    t.time = getTime();
    t.which_variant = meshtastic_Telemetry_environment_metrics_tag;
    t.variant.environment_metrics = meshtastic_EnvironmentMetrics_init_zero;
    t.variant.environment_metrics.has_temperature = true;
    t.variant.environment_metrics.temperature = tempC;
    t.variant.environment_metrics.has_relative_humidity = true;
    t.variant.environment_metrics.relative_humidity = rh;

    meshtastic_MeshPacket *p = allocDataPacket(); // portnum defaults to ours — override to TELEMETRY_APP
    if (!p)
        return;
    p->decoded.portnum = meshtastic_PortNum_TELEMETRY_APP;
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_Telemetry_msg, &t);
    if (g_cfg.tx.dest_node)
        p->to = g_cfg.tx.dest_node;
    p->channel = g_cfg.tx.channel;
    LOG_INFO("SensorTH: tx EnvironmentMetrics T=%.2fC RH=%.1f%% on TELEMETRY_APP(67)", (double)tempC, (double)rh);
    service->sendToMesh(p, RX_SRC_LOCAL, true);
}

void SensorTHModule::buildAndSend(float tempC, float rh, bool heaterUsed, bool ok)
{
    uint8_t buf[SQS_TH_PAYLOAD_LEN];
    int16_t t = (int16_t)lroundf(tempC * 100.0f);
    uint16_t h = (uint16_t)lroundf(rh * 100.0f);
    uint16_t vbat = 0; // M4: battery telemetry
    uint8_t flags = 0;
    if (!ok)
        flags |= SQS_FLAG_SENSOR_NACK;
    if (heaterUsed)
        flags |= SQS_FLAG_HEATER_RAN;

    buf[0] = SQS_TH_PAYLOAD_VER;
    buf[1] = SQS_MODEL_SQS_TH_I;
    buf[2] = (uint8_t)(t & 0xFF);
    buf[3] = (uint8_t)((t >> 8) & 0xFF);
    buf[4] = (uint8_t)(h & 0xFF);
    buf[5] = (uint8_t)((h >> 8) & 0xFF);
    buf[6] = (uint8_t)(vbat & 0xFF);
    buf[7] = (uint8_t)((vbat >> 8) & 0xFF);
    buf[8] = 0xFF; // soc unknown (M4)
    buf[9] = flags;
    buf[10] = (uint8_t)(sqsBootCount & 0xFF);

    meshtastic_MeshPacket *p = allocDataPacket(); // portnum set to ours by SinglePortModule
    if (!p)
        return;
    // tx target from config: unicast to a chosen collector (want_ack if confirmed) or
    // broadcast (the allocDataPacket default); on the chosen mesh channel.
    if (g_cfg.tx.dest_node)
        p->to = g_cfg.tx.dest_node;
    p->channel = g_cfg.tx.channel;
    p->want_ack = g_cfg.tx.confirmed && g_cfg.tx.dest_node;
    memcpy(p->decoded.payload.bytes, buf, sizeof(buf));
    p->decoded.payload.size = sizeof(buf);

    LOG_INFO("SensorTH: tx T=%.2fC RH=%.1f%% flags=0x%02X boot=%u on portnum %u", (double)tempC, (double)rh,
             flags, (unsigned)sqsBootCount, (unsigned)SILIQS_SENSOR_PORTNUM);
    // ccToPhone=true so a USB/BLE-attached configurator sees this node's own reading
    // (its mesh broadcasts don't otherwise reach the local API). Harmless with no client.
    service->sendToMesh(p, RX_SRC_LOCAL, true);
}

int32_t SensorTHModule::runOnce()
{
    if (firstTime) {
        firstTime = false;
        sqsBootCount++;
        Wire.begin(I2C_SDA, I2C_SCL);
        Wire.setClock(400000);
        sensorOk = initSensor();
        // Boot de-condensation heater, matching the validated driver's setup() — but only on
        // a REAL power-on (VBUS cycled / reset button), not a duty-cycle timer wake. In deep
        // sleep, esp_deep_sleep_start() is a full reboot, so without this guard firstTime is
        // true on every wake and this ran every reportIntervalMs() cycle (default 60s): a
        // needless 1.2s heater + 200ms settle each time, not just the >=95% RH case
        // takeReadingAndSend() already gates on. Costs battery life and can bias the very
        // reading it's supposed to protect.
        if (sensorOk && esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER)
            runHeater();
        LOG_INFO("SensorTH: report interval %us (from config), deep_sleep %d", (unsigned)g_cfg.power.uplink_interval_s,
                 (int)g_cfg.power.deep_sleep);
    }

    // One-shot: on a reset / power-on (not a duty-cycle timer wake), open a grace window
    // so a configurator can attach to a sleepy node before it powers down again.
    if (!bootChecked) {
        bootChecked = true;
        if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER)
            connectGraceUntil = millis() + SQS_CONNECT_GRACE_MS;
    }

    // Deep-sleep duty cycle: a reading was enqueued last cycle — power down now, once the
    // send has settled (split across runOnce calls so we don't cut power mid-transmit).
    if (sleepArmed) {
        if (clientConnected() || millis() < connectGraceUntil) {
            sleepArmed = false; // a configurator is attached / still expected — stay awake
            return reportIntervalMs();
        }
        uint32_t waited = millis() - sleepArmedAt;
        bool sending = RadioLibInterface::instance && RadioLibInterface::instance->isSending();
        if (waited < SQS_L4_MIN_DRAIN_MS || (sending && waited < SQS_L4_MAX_DRAIN_MS))
            return 250;         // still draining — re-check shortly
        enterDeepSleep();       // radio + MCU down for the interval; never returns
    }

    takeReadingAndSend();

    // Power-save leaf: arm the deep sleep, then let the next runOnce power us down once
    // the broadcast has drained. Otherwise just wait out the interval awake (always-on /
    // relay). enterDeepSleep() reboots on wake, so the cycle restarts at firstTime.
    if (sleepMode()) {
        sleepArmed = true;
        sleepArmedAt = millis();
        return 250;
    }
    return reportIntervalMs();
}

// Power-save leaf gate: deep_sleep configured AND a non-relaying CLIENT_MUTE role (a
// CLIENT/router that slept would stop relaying for the mesh). SQS-TH-I factory-defaults
// to CLIENT_MUTE (see NodeDB installDefaultConfig); set a relay role to keep it awake.
int SensorTHModule::sleepMode()
{
    if (g_cfg.power.deep_sleep && config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE)
        return 4;
    return 0;
}

void SensorTHModule::enterDeepSleep()
{
    LOG_INFO("SensorTH: deep sleep %us (power-save leaf) — radio + MCU down, reboots on wake",
             (unsigned)g_cfg.power.uplink_interval_s);
    if (RadioLibInterface::instance)
        RadioLibInterface::instance->sleep(); // SX126x → sleep (µA); full reinit on reboot
    esp_sleep_enable_timer_wakeup((uint64_t)g_cfg.power.uplink_interval_s * 1000000ULL);
    esp_deep_sleep_start(); // MCU powers down; no return
}

// Defer sleep while something is actively using the node over USB/BLE:
//   1) a Meshtastic API client is attached (post-handshake), OR
//   2) a USB host has the native-USB CDC open (DTR asserted). (2) also covers the window
//      before the Meshtastic handshake completes — a flasher/configurator/monitor holding
//      the port keeps the node awake instead of sleeping mid-handshake, and keeps a
//      USB-tethered bench board reachable. On a dumb USB power source (no host, DTR low)
//      the node still sleeps, so battery behaviour is unchanged.
bool SensorTHModule::clientConnected()
{
    if (service && service->api_state != MeshService::STATE_DISCONNECTED)
        return true;
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    if (Serial) // HWCDC::operator bool() — a USB host has the CDC open (DTR)
        return true;
#endif
    return false;
}

// ── provisioning: accept a config blob on our PortNum ──────────────────────
// Mirrors the SQC485Iv2 'SQ'+blob path so the same Configurator flow sets the SQS
// node's report interval / tx target. config_from_blob() validates magic+ver+CRC.
ProcessMessage SensorTHModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (mp.decoded.portnum != ourPortNum)
        return ProcessMessage::CONTINUE;
    const uint8_t *b = mp.decoded.payload.bytes;
    size_t n = mp.decoded.payload.size;

    // Capability / identity query: 'S','Q','V','?' → reply product_id + features (mirrors
    // ModbusModule). This is what lets the Configurator AUTO-DETECT the node type: a sensor
    // node answers with product_id "SQS-TH-I" and the SQ_FEAT_SENSOR bit set, so the UI hides
    // the Modbus fields and shows only the report interval.
    if (n >= 4 && b[0] == 'S' && b[1] == 'Q' && b[2] == 'V' && b[3] == '?') {
        const char *fw = SQ_FW_VERSION;
        const char *pid = SQ_PRODUCT_ID;
        uint8_t fl = (uint8_t)strlen(fw);
        uint8_t pdl = (uint8_t)strlen(pid);
        uint8_t r[9 + sizeof(SQ_FW_VERSION) + sizeof(SQ_PRODUCT_ID)];
        size_t i = 0;
        r[i++] = 'S'; r[i++] = 'Q'; r[i++] = 'V';
        r[i++] = SQ_CAP_PROTO;
        r[i++] = SQ_CONFIG_VERSION;
        r[i++] = SQ_FEATURES;
        r[i++] = fl;
        memcpy(r + i, fw, fl); i += fl;
        r[i++] = pdl;
        memcpy(r + i, pid, pdl); i += pdl;
        LOG_INFO("SensorTH: capability query → %s fw %s, feat 0x%02x", pid, fw, (unsigned)SQ_FEATURES);
        sendReply(r, i, mp.from);
        return ProcessMessage::CONTINUE;
    }

    // Poll-now: 'S','Q','?' → read the sensor and send immediately (configurator "read now").
    if (n >= 3 && b[0] == 'S' && b[1] == 'Q' && b[2] == '?') {
        LOG_INFO("SensorTH: poll-now request");
        takeReadingAndSend();
        return ProcessMessage::CONTINUE;
    }

    // Get-config: 'S','Q','G','?' → reply the CURRENT config blob so the configurator can
    // read back / pre-fill the report interval.
    if (n >= 4 && b[0] == 'S' && b[1] == 'Q' && b[2] == 'G' && b[3] == '?') {
        uint8_t r[3 + 160];
        r[0] = 'S'; r[1] = 'Q'; r[2] = 'G';
        size_t bl = config_to_blob(&g_cfg, r + 3, sizeof(r) - 3);
        if (bl)
            sendReply(r, 3 + bl, mp.from);
        return ProcessMessage::CONTINUE;
    }

    // Config write: 'SQ' + blob (small version byte 2..15, validated by config_from_blob).
    // The ASCII command/reply markers above (byte[2] >= 0x20) never reach here.
    if (n < 6 || b[0] != 'S' || b[1] != 'Q' || b[2] < 2 || b[2] > 15)
        return ProcessMessage::CONTINUE;
    applyConfigBlob(b, n, mp.from);
    return ProcessMessage::CONTINUE;
}

void SensorTHModule::applyConfigBlob(const uint8_t *blob, size_t len, uint32_t from)
{
    // status: 0 = applied, 1 = invalid (magic/ver/len/CRC), 2 = valid but save failed
    uint8_t status;
    sq_config_t incoming = g_cfg; // preserve fields the blob doesn't carry
    if (!config_from_blob(&incoming, blob, len)) {
        status = 1;
    } else if (!config_save(&incoming)) {
        status = 2;
    } else {
        g_cfg = incoming;
        // Restart the thread's timer — same bug and fix as ModbusModule::applyConfigBlob().
        // Without this the node is still sleeping out whatever runOnce() last returned (the
        // OLD interval), so a shortened cadence would not take effect until the old one
        // expired: an hourly reader moved to minutely would stay silent for up to an hour.
        // Skip while sleepArmed: a reading is already draining before deep-sleep, and
        // enterDeepSleep() reads g_cfg.power.uplink_interval_s fresh when it fires, so the
        // new interval takes effect at the next wake regardless — restarting the thread
        // schedule here would only delay the pending drain-and-sleep check.
        if (!sleepArmed)
            setIntervalFromNow(reportIntervalMs());
        LOG_INFO("SensorTH: config updated over mesh from 0x%08x — interval %us, deep_sleep %d",
                 (unsigned)from, (unsigned)g_cfg.power.uplink_interval_s, (int)g_cfg.power.deep_sleep);
        status = 0;
    }
    const uint8_t r[4] = {'S', 'Q', '!', status};
    sendReply(r, sizeof(r), from);
}

void SensorTHModule::sendReply(const uint8_t *data, size_t len, uint32_t to)
{
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return;
    if (to)
        p->to = to;
    p->channel = g_cfg.tx.channel;
    memcpy(p->decoded.payload.bytes, data, len);
    p->decoded.payload.size = len;
    service->sendToMesh(p, RX_SRC_LOCAL, true); // cc to the local USB/BLE configurator too
}

#endif // SQS_TH_I
