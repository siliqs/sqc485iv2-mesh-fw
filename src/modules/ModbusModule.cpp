/*
 * ModbusModule.cpp — glue between Meshtastic and the shared firmware_core engine.
 *
 * The Modbus transaction, retry/error-frame logic and raw-forward payload are NOT
 * reimplemented here — they are the same portable C the Star (LoRaWAN) version
 * uses, backed in this build by hal_meshtastic.cpp (Serial1 / millis / LittleFS):
 *     firmware_core/src/{modbus,poll,config}.c
 *
 * Differences vs 16_'s PLCModbusModule (which this de-brands + generalizes):
 *   - poll plan / baud / interval come from the config blob, not #defines;
 *   - payload is the variable-length RAW FORWARD (concatenated [addr][func][data],
 *     per-poll error frame on failure) — the cloud decodes it by config. (The 16_
 *     node wrapped this in a fixed 51-byte Rs485Payload for the project-08 USB
 *     receiver; the generic cloud consumes the raw forward directly.)
 *   - a config blob can be pushed over the mesh on our PortNum (companion app),
 *     not only over the serial console.
 *
 * Register in src/modules/Modules.cpp setupModules():
 *     modbusModule = new ModbusModule();
 */
#include "ModbusModule.h"

#ifdef SQC485IV2

#include "MeshService.h"
#include "NodeDB.h"
#include "configuration.h"
#include "main.h"
#include "Router.h"              // generatePacketId() — Level-3 confirmed uplink id
#include "RadioLibInterface.h"   // instance->isSending()/sleep() — Level-4 TX drain + radio sleep
#include <NimBLEDevice.h>   // NimBLEDevice::setPower (BLE TX power control)
#include <esp_sleep.h>      // esp_deep_sleep_start (Epic G Level 4 duty-cycle sleep, #9)

extern "C" {
#include "config.h"          // sq_config_t, config_load/save, config_from_blob
#include "poll.h"            // poll_collect_raw  (raw-forward payload)
#include "sqcmd.h"           // sq_classify + the reply builders (host-tested, no state)
#include "hal/hal_serial.h"  // hal_serial_init + raw write/read (USB↔RS485 bridge)
#include "hal/hal_time.h"    // hal_millis (idle-gap framing for the RS485 tunnel)
#include "hal/hal_store.h"   // persist the BLE TX power (separate key, not the blob)
#include "board_profile.h"   // BOARD.rs485_tx_echo (strip half-duplex TX echo)
void hal_led_idle_on(void);   // meshtastic HAL (hal_meshtastic.cpp): keep the status LED on while idle
}

ModbusModule *modbusModule;

// Shared config (loaded from our LittleFS store via hal_store in hal_meshtastic.cpp).
static sq_config_t g_cfg;

// Epic G Level 4 (#9): TX-drain window before deep sleep. MIN covers Meshtastic's
// CSMA/tx-delay (~0.5–1s) so the broadcast actually starts before we cut power; MAX
// is a ceiling so a wedged radio can't block the power-down indefinitely.
static const uint32_t SQ_L4_MIN_DRAIN_MS = 1500;
static const uint32_t SQ_L4_MAX_DRAIN_MS = 8000;

// After a reset / power-on (someone plugged in USB or pressed reset), keep a deep-sleep
// leaf awake this long so a configurator has time to connect before the first sleep. A
// duty-cycle timer wake gets no grace (keeps the sensor cadence tight).
static const uint32_t SQ_CONNECT_GRACE_MS = 30000;

// Epic G Level 3 (#10): max time to stay awake waiting for the confirmed-uplink ACK
// before sleeping anyway. Covers ReliableRouter's NUM_RELIABLE_RETX (3) retransmits;
// an early ACK sleeps sooner. If the collector is unreachable we burn this each cycle
// — the documented cost of confirmed + deep sleep.
static const uint32_t SQ_L3_ACK_WAIT_MS = 12000;

// Tunnel source/sink. Default = the local RS485 (Serial1 via the HAL). In the
// SQ_USB_TUNNEL build the source/sink is the USB CDC (Serial) instead — the console
// is moved off USB (USER_DEBUG_PORT=g_nullStream) so the port is free for raw data.
// The mesh side (forwardTunnel '/SQ}', peer reply '/SQ{') is identical either way.
#ifdef SQ_USB_TUNNEL
static inline int  tun_read(uint8_t *buf, size_t len) {
    size_t n = 0;
    while (n < len && Serial.available()) buf[n++] = (uint8_t)Serial.read();
    return (int)n;
}
static inline void tun_write(const uint8_t *buf, size_t len) { Serial.write(buf, len); }
#else
static inline int  tun_read(uint8_t *buf, size_t len) { return hal_serial_read(buf, len, 2); }
static inline void tun_write(const uint8_t *buf, size_t len) {
    hal_serial_set_tx(true);
    hal_serial_write(buf, len);
    hal_serial_flush();
    hal_serial_set_tx(false);
    // Discard our own TX echo so the framer doesn't re-forward it (half-duplex RS485).
    if (BOARD.rs485_tx_echo) {
        uint8_t scratch[64];
        size_t echo = len;
        while (echo) {
            int n = hal_serial_read(scratch, echo < sizeof(scratch) ? echo : sizeof(scratch), 50);
            if (n <= 0) break;
            echo -= (size_t)n;
        }
    }
}
#endif

ModbusModule::ModbusModule()
    : SinglePortModule("modbus", SILIQS_MODBUS_PORTNUM),
      concurrency::OSThread("Modbus")
{
    config_load(&g_cfg);     // defaults if first boot / unprovisioned
}

int32_t ModbusModule::runOnce()
{
    // One-shot at startup: re-apply the saved BLE TX power (BLE is up by now). Done
    // here (not in main) to keep the change in our module; harmless if never set.
    if (!blePowerApplied) {
        blePowerApplied = true;
        int8_t dbm;
        if (hal_store_get("blepwr", &dbm, sizeof(dbm)) == (int)sizeof(dbm))
            applyBlePower((int)dbm, false);
    }

    // One-shot: on a reset / power-on (not a duty-cycle timer wake), open a grace window
    // during which we won't deep-sleep — so a configurator can connect to a sleep node
    // before it powers down again.
    if (!bootChecked) {
        bootChecked = true;
        if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER)
            connectGraceUntil = hal_millis() + SQ_CONNECT_GRACE_MS;
    }

    // Epic G duty-cycle deep sleep: a deep-sleep uplink was enqueued last cycle — now
    // power down, once the send has settled. Split across runOnce() calls so we don't
    // cut power mid-transmit. Level 3 waits for the confirmed-delivery ACK (early-out)
    // or its retransmission window; Level 4 waits for the broadcast TX to drain.
    if (sleepArmed) {
        // A configurator attached (or is still expected during the connect grace) after
        // we armed — cancel the sleep and stay awake so it can reconfigure the node.
        if (clientConnected() || hal_millis() < connectGraceUntil) {
            sleepArmed = false; ackWaitId = 0;
            return (int32_t)g_cfg.power.uplink_interval_s * 1000;
        }
        uint32_t waited = hal_millis() - sleepArmedAt;
        if (ackWaitId) {   // Level 3: confirmed unicast — wait for the ACK
            if (!ackReceived && waited < SQ_L3_ACK_WAIT_MS)
                return 250;          // still awaiting ACK / retransmitting
            LOG_INFO("ModbusModule: Level-3 uplink %s after %ums — sleeping",
                     ackReceived ? "ACKed" : "NOT acked (retries exhausted)", (unsigned)waited);
        } else {           // Level 4: fire-and-forget — wait for TX to drain
            bool sending = RadioLibInterface::instance && RadioLibInterface::instance->isSending();
            if (waited < SQ_L4_MIN_DRAIN_MS || (sending && waited < SQ_L4_MAX_DRAIN_MS))
                return 250;          // still draining — re-check shortly
        }
        enterDeepSleep();           // sleeps radio + MCU for the interval; never returns
    }

    // Polling is gated by the config flag alone (g_cfg.rs485_enabled, below) — NOT by
    // the mesh role. This lets a CLIENT_MUTE node still poll: a "mute sensor" (Epic G
    // Level 2, #8) that sends its own readings but does NOT relay for the mesh. A
    // gateway is just CLIENT_MUTE with rs485_enabled=false, so it still stays idle at
    // the rs485_enabled check below (which already supports re-enable without reboot).
    // (Was: force-disable whenever role==CLIENT_MUTE — that assumed CLIENT_MUTE==gateway.)

    // Tunnel master: instead of polling, read raw frames off the local port and
    // forward them to the peer (its reply is written back in handleReceived). The
    // local port is RS485 normally, or the USB CDC in the SQ_USB_TUNNEL build.
    if (g_cfg.tunnel.enabled) {
        if (firstTime) {
            firstTime = false;
#ifdef SQ_USB_TUNNEL
            Serial.begin(g_cfg.modbus.baud);   // USB CDC: baud is cosmetic, ensures open
            LOG_INFO("ModbusModule: USB<->USB pipe <-> node 0x%08x (gap %ums)",
                     (unsigned)g_cfg.tunnel.peer_node, (unsigned)g_cfg.tunnel.idle_gap_ms);
#else
            hal_serial_init(g_cfg.modbus.baud, g_cfg.modbus.parity, g_cfg.modbus.stop_bits);
            LOG_INFO("ModbusModule: RS485 tunnel master @%u baud -> node 0x%08x (gap %ums)",
                     (unsigned)g_cfg.modbus.baud, (unsigned)g_cfg.tunnel.peer_node,
                     (unsigned)g_cfg.tunnel.idle_gap_ms);
#endif
            tunLen = 0;
        }
        return tunnelPump();
    }

    // RS485 polling turned off (e.g. no sensor wired) — stay idle, but keep checking so a config
    // push can re-enable it without a reboot. Keep the status LED on (GPIO2 held LOW) so an idle
    // node still shows a lit LED instead of going dark.
    if (!g_cfg.rs485_enabled) {
        hal_led_idle_on();
        return 5000;
    }

    if (firstTime) {
        firstTime = false;
        // Bring up RS485 at the configured link params (one-shot; not per-poll).
        hal_serial_init(g_cfg.modbus.baud, g_cfg.modbus.parity, g_cfg.modbus.stop_bits);
        // Priming transaction: the very first RS485 read can mis-align while the
        // line/echo settles. Discard it so the first forwarded packet is clean.
        uint8_t prime[64];
        (void)poll_collect_raw(&g_cfg, prime, sizeof(prime));
        LOG_INFO("ModbusModule: RS485 up %u 8N1, %u poll(s); priming done, first uplink in 3s",
                 (unsigned)g_cfg.modbus.baud, g_cfg.poll_count);
        return 3000;
    }

    pollAndSend();

    // The uplink is enqueued; if this is a sleeping leaf, arm deep sleep and let the
    // send settle (handled at the top of the next runOnce) rather than staying awake
    // for the whole interval. Only a CLIENT_MUTE leaf sleeps — see sleepMode().
    // BUT keep the node awake while a configurator is attached (or during the post-reset
    // connect grace): poll on schedule but don't sleep, so it can be reconfigured. It
    // resumes its configured duty-cycle sleep once the configurator disconnects.
    if (sleepMode()) {
        if (clientConnected() || hal_millis() < connectGraceUntil) {
            if (!sleepSuppressedLogged) {
                LOG_INFO("ModbusModule: configurator attached — deep sleep deferred until you disconnect");
                sleepSuppressedLogged = true;
            }
        } else {
            sleepSuppressedLogged = false;
            sleepArmed   = true;
            sleepArmedAt = hal_millis();
            return 250;
        }
    }

    // Mesh airtime is precious — the configured interval governs cadence.
    return (int32_t)g_cfg.power.uplink_interval_s * 1000;
}

// True while a configurator/client is attached over the API (USB serial or BLE).
// Meshtastic sets service->api_state to STATE_SERIAL/STATE_BLE while a client is
// connected and back to STATE_DISCONNECTED when it closes or times out — so this
// flips false shortly after the configurator disconnects, letting the node sleep.
bool ModbusModule::clientConnected()
{
    return service && service->api_state != MeshService::STATE_DISCONNECTED;
}

// Epic G duty-cycle deep sleep applies only to a non-relaying leaf: deep_sleep
// configured AND role CLIENT_MUTE (a CLIENT/router that slept would drop the mesh)
// AND RS485 on (we only sleep after a real poll). Returns which flavour:
//   3 = confirmed  (#10): unicast want_ack to dest_node, sleep after ACK/retries.
//   4 = fire-and-forget (#9): broadcast, sleep after TX drains.
//   0 = don't sleep. Confirmed needs a unicast dest_node (can't ack a broadcast) —
// without one a "confirmed" config degrades to Level 4.
int ModbusModule::sleepMode()
{
    if (!(g_cfg.power.deep_sleep && g_cfg.rs485_enabled &&
          config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE))
        return 0;
    return (g_cfg.tx.confirmed && g_cfg.tx.dest_node) ? 3 : 4;
}

// Sleep the radio, then deep-sleep the MCU for the configured interval. On the ESP32
// timer wake the chip reboots (setup() runs again) — so this never returns, and the
// next boot polls once and sleeps again. Meshtastic's own sleep FSM is bypassed; this
// path is reached only for a Level-4 leaf (level4Active()).
void ModbusModule::enterDeepSleep()
{
    LOG_INFO("ModbusModule: Level-4 deep sleep %us (CLIENT_MUTE leaf) — radio+MCU down, reboots on wake",
             (unsigned)g_cfg.power.uplink_interval_s);
    if (RadioLibInterface::instance)
        RadioLibInterface::instance->sleep();     // SX126x → sleep (µA), full reinit on reboot
    esp_sleep_enable_timer_wakeup((uint64_t)g_cfg.power.uplink_interval_s * 1000000ULL);  // s → µs
    esp_deep_sleep_start();                        // MCU powers down; no return
}

void ModbusModule::pollAndSend()
{
    // SHARED engine: raw-forward payload — concatenated [addr][func][data] across
    // the poll list; a failed poll contributes a same-length Modbus error frame
    // ([slave][func|0x80][err…]). Never empty when poll_count > 0.
    uint8_t payload[meshtastic_Constants_DATA_PAYLOAD_LEN];
    size_t  len = poll_collect_raw(&g_cfg, payload, sizeof(payload));
    if (len == 0)
        return;

    meshtastic_MeshPacket *p = allocDataPacket();   // portnum set to ours by SinglePortModule
    if (!p)
        return;
    // Telemetry destination (config v3): unicast to a chosen node, or broadcast
    // (the allocDataPacket default); and the chosen mesh channel index.
    if (g_cfg.tx.dest_node)
        p->to = g_cfg.tx.dest_node;
    p->channel = g_cfg.tx.channel;
    // Level 3 (#10): confirmed unicast — ask the mesh for a reliable-delivery ACK and
    // remember this packet's id so we can watch for that ACK (ROUTING_APP, request_id)
    // in handleReceived and sleep as soon as it lands. Only meaningful with a dest.
    bool confirmed = g_cfg.tx.confirmed && g_cfg.tx.dest_node;
    p->want_ack = confirmed;
    if (confirmed) {
        p->id       = generatePacketId();
        ackWaitId   = p->id;
        ackReceived = false;
    } else {
        ackWaitId = 0;
    }
    memcpy(p->decoded.payload.bytes, payload, len);
    p->decoded.payload.size = len;
    // Log the payload hex (capped) — lets the installer see the actual forwarded
    // bytes on the console; a node's own mesh broadcasts don't always reach the
    // phone API, so this is the reliable bench read-back.
    char hx[2 * 32 + 1];
    size_t hn = len < 32 ? len : 32;
    for (size_t i = 0; i < hn; i++)
        snprintf(hx + 2 * i, 3, "%02x", payload[i]);
    hx[2 * hn] = 0;
    LOG_INFO("ModbusModule: tx raw-forward %u bytes on portnum %u: %s", (unsigned)len,
             (unsigned)SILIQS_MODBUS_PORTNUM, hx);
    // ccToPhone=true so a USB/BLE-connected configurator (which is this node's own
    // "phone") can see this node's reads — its mesh broadcasts don't otherwise reach
    // the local API. Harmless when no client is attached.
    service->sendToMesh(p, RX_SRC_LOCAL, true);   // broadcast + cc to the local client
}

ProcessMessage ModbusModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Level 3 (#10): the reliable-delivery ACK for our confirmed uplink comes back on
    // ROUTING_APP, addressed to us, with request_id == the id we sent. Catching it lets
    // enterDeepSleep happen as soon as delivery is confirmed instead of waiting out the
    // full retransmission window. (wantPacket() opts us in to ROUTING_APP for this.)
    if (mp.decoded.portnum == meshtastic_PortNum_ROUTING_APP) {
        if (ackWaitId && mp.decoded.request_id == ackWaitId) {
            ackReceived = true;
            LOG_INFO("ModbusModule: Level-3 ACK for uplink id 0x%08x", (unsigned)ackWaitId);
        }
        return ProcessMessage::CONTINUE;
    }

    const uint8_t *b = mp.decoded.payload.bytes;
    size_t n = mp.decoded.payload.size;

    // The classification below is byte-pattern only, so it lives in firmware_core
    // (sqcmd.c) where the host tests can reach every branch — the marker/blob
    // collisions and the "never act on our own reply" rule are exactly the kind of
    // thing that is expensive to discover on a mesh. What stays here is what needs
    // node state: who to answer, whether the tunnel is up, who the peer is.
    const sq_cmd_t cmd = sq_classify(b, n);
    const uint32_t self = nodeDB->getNodeNum();

    switch (cmd) {

    // Poll-now test request: the 3 bytes 'S','Q','?'. Do an immediate read and
    // send it the SAME way as a periodic read — pollAndSend() broadcasts + cc's to
    // the phone, which reliably reaches a connected configurator. (An addressed
    // self-reply was unreliable; the broadcast path is the one that works.)
    case SQ_CMD_POLL_NOW:
        LOG_INFO("ModbusModule: poll-now request — reading + broadcasting");
        pollAndSend();
        break;

    // BLE TX power: 'S','Q','P' + int8 dBm. Set the radio power live and persist it to
    // a separate key (not the config blob). No reboot.
    case SQ_CMD_BLE_POWER:
        applyBlePower((int)(int8_t)b[3], true);
        break;

    // Capability query → the report the configurator uses to show the real product +
    // firmware and gate/verify features.
    case SQ_CMD_CAPABILITY: {
        uint8_t r[SQ_CAP_REPLY_MAX];
        size_t len = sq_build_capability_reply(r, sizeof(r));
        if (len) {
            LOG_INFO("ModbusModule: capability query → %s fw %s, blob v%u, feat 0x%02x",
                     SQ_PRODUCT_ID, SQ_FW_VERSION, (unsigned)SQ_CONFIG_VERSION, (unsigned)SQ_FEATURES);
            sendSqReply(r, len, mp.from, mp.channel);
        }
        break;
    }

    // Get-config query → the CURRENT config blob, so the configurator can read back
    // what the node is actually set to and pre-fill its form instead of guessing.
    case SQ_CMD_GET_CONFIG: {
        uint8_t r[3 + 160];
        size_t len = sq_build_get_config_reply(&g_cfg, r, sizeof(r));
        if (len) {
            LOG_INFO("ModbusModule: get-config query → %u-byte config blob", (unsigned)(len - 3));
            sendSqReply(r, len, mp.from, mp.channel);
        }
        break;
    }

    // RS485 raw-bridge request: write the following bytes to RS485 and return the
    // reply. Two distinct callers share this path but use DIFFERENT reply markers so
    // they never cross (e.g. when both target the same peer):
    //   'S','Q','>'  USB↔RS485 tool (manual / configurator)  → reply 'S','Q','<'
    //   'S','Q','}'  RS485↔RS485 tunnel (autonomous master)  → reply 'S','Q','{'
    case SQ_CMD_RS485_BRIDGE:
    case SQ_CMD_TUNNEL_FORWARD:
        // Only act if this request is for US: addressed to our node or broadcast.
        // Otherwise it's a packet the local node merely originates — transmit it,
        // don't run it against our own RS485.
        if (mp.to == self || mp.to == NODENUM_BROADCAST) {
            // Local injection (from == self) → reply to USB only (no RF). A genuine
            // remote requester → reply over the mesh, unicast back on its channel.
            const bool local = (mp.from == 0 || mp.from == self);
            rawBridge(b + SQ_FRAME_PAYLOAD_OFFSET, n - SQ_FRAME_PAYLOAD_OFFSET, mp.from, mp.channel, local,
                      sq_bridge_reply_marker(cmd));
        }
        break;

    // RS485↔RS485 tunnel reply: our peer answered a forwarded frame with 'SQ{' + raw
    // bytes. Write them straight back out our LOCAL RS485 to the master. Only when we
    // are the tunnel master and only from our configured peer. The distinct '{' marker
    // (vs the USB tool's '<') means a manual USB probe of the same peer is NOT mistaken
    // for a tunnel reply and injected onto our bus.
    case SQ_CMD_TUNNEL_REPLY:
        if (g_cfg.tunnel.enabled && mp.from == g_cfg.tunnel.peer_node)
            tunnelWriteback(b + SQ_FRAME_PAYLOAD_OFFSET, n - SQ_FRAME_PAYLOAD_OFFSET);
        break;

    // USB↔USB pipe: the peer pushed data 'SQ~' + raw bytes — write it straight to our
    // local USB. Symmetric (both ends do this); no reply.
    case SQ_CMD_PIPE_DATA:
#ifdef SQ_USB_TUNNEL
        if (g_cfg.tunnel.enabled && mp.from == g_cfg.tunnel.peer_node)
            tunnelWriteback(b + SQ_FRAME_PAYLOAD_OFFSET, n - SQ_FRAME_PAYLOAD_OFFSET);
#endif
        break;

    // Config downlinks and our own telemetry uplinks share this PortNum, and we
    // CANNOT tell them apart by source: a companion app provisions by injecting the
    // config on the LOCAL node's Meshtastic API, so its packet is from==self, exactly
    // like our telemetry loopback. Only CONTENT decides — config_from_blob() still has
    // the final say via magic + version + length + CRC16.
    case SQ_CMD_CONFIG_BLOB:
        applyConfigBlob(b, n, mp.from);   // applies + replies 'SQ!' with the apply status
        break;

    // Our own 'SQ<' / 'SQ{' / 'SQ!' / 'SQV' replies, and raw-forward telemetry: not
    // commands. Dropping them here is what stops a reply loop and a spurious NAK for
    // every packet we send.
    case SQ_CMD_OWN_REPLY:
    case SQ_CMD_IGNORE:
        break;
    }

    return ProcessMessage::CONTINUE;
}

void ModbusModule::applyConfigBlob(const uint8_t *blob, size_t len, uint32_t from)
{
    // Status codes echoed back to the configurator in the 'SQ!' reply — see the
    // SQ_CFG_* constants in sqcmd.h.
    uint8_t status;
    sq_config_t incoming = g_cfg;   // preserve fields the blob doesn't carry (LoRaWAN keys)
    size_t plan_len = 0;
    if (!config_from_blob(&incoming, blob, len)) {
        LOG_DEBUG("ModbusModule: rx %u bytes on portnum not a valid config blob; NAK",
                  (unsigned)len);
        status = SQ_CFG_INVALID;
    } else if ((plan_len = sq_plan_payload_len(&incoming)) > meshtastic_Constants_DATA_PAYLOAD_LEN) {
        // Refuse rather than accept-and-truncate. poll_collect_raw() would drop
        // whole polls off the end silently, so the operator would see a plan that
        // "applied" while its last datapoints never appeared in any uplink.
        LOG_WARN("ModbusModule: config rejected — %u poll(s) need %u bytes, a packet carries %u",
                 incoming.poll_count, (unsigned)plan_len,
                 (unsigned)meshtastic_Constants_DATA_PAYLOAD_LEN);
        status = SQ_CFG_PLAN_TOO_LARGE;
    } else if (!config_save(&incoming)) {
        LOG_WARN("ModbusModule: config blob valid but save failed");
        status = SQ_CFG_NOT_PERSISTED;
    } else {
        g_cfg = incoming;
        // Re-init the UART so a changed baud/parity takes effect without a reboot.
        hal_serial_init(g_cfg.modbus.baud, g_cfg.modbus.parity, g_cfg.modbus.stop_bits);
        LOG_INFO("ModbusModule: config updated over mesh from 0x%08x — %u poll(s), %u byte payload, baud %u",
                 (unsigned)from, g_cfg.poll_count, (unsigned)plan_len, (unsigned)g_cfg.modbus.baud);
        status = SQ_CFG_OK;
    }
    // App-level ACK/NAK so the configurator knows the node actually accepted it
    // (older firmware without this simply won't reply → the UI shows "unconfirmed").
    uint8_t r[SQ_CONFIG_ACK_LEN];
    sendSqReply(r, sq_build_config_ack(status, r, sizeof(r)), from, 0);
}

void ModbusModule::sendSqReply(const uint8_t *data, size_t len, uint32_t from, uint8_t channel)
{
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return;
    p->want_ack = false;
    memcpy(p->decoded.payload.bytes, data, len);
    p->decoded.payload.size = len;
    const uint32_t self = nodeDB->getNodeNum();
    if (from == 0 || from == self) {
        // Provisioned locally over USB/BLE → deliver straight to the client, no RF.
        p->from = self;
        service->sendToPhone(p);
    } else {
        // Provisioned by a remote node over the mesh → unicast the reply back to it.
        p->to = from;
        p->channel = channel;
        service->sendToMesh(p, RX_SRC_LOCAL, true);
    }
}

void ModbusModule::rawBridge(const uint8_t *req, size_t reqlen, uint32_t replyTo, uint8_t channel,
                             bool local, uint8_t replyMarker)
{
    // The request carries its own link params so the converter is independent of
    // the polling config: a 6-byte header  baud(u32 LE) parity(u8) stop(u8)  then
    // the raw bytes to put on the wire. (Shorter request ⇒ legacy: use g_cfg.)
    // Parsing lives in firmware_core/sqcmd.c — the "6 or more bytes means header"
    // rule silently mis-reads a bare Modbus frame, so it is pinned by a host test.
    sq_bridge_req_t br;
    sq_parse_bridge_request(req, reqlen, &g_cfg.modbus, &br);
    const uint32_t     baud    = br.baud;
    const hal_parity_t par     = (hal_parity_t)br.parity;
    const uint8_t      stop    = br.stop_bits;
    const uint8_t     *data    = br.frame;
    const size_t       datalen = br.frame_len;

    // Bring the RS485 UART up at the requested link params (the periodic poller may
    // be disabled or not yet have run, but the bridge must work regardless).
    hal_serial_init(baud, par, stop);

    // Drive DE, put the caller's bytes on the wire, then drop back to receive.
    hal_serial_set_tx(true);
    if (datalen)
        hal_serial_write(data, datalen);
    hal_serial_flush();
    hal_serial_set_tx(false);

    const uint32_t to = g_cfg.modbus.response_timeout_ms ? g_cfg.modbus.response_timeout_ms : 500;

    // On half-duplex boards that echo TX (/RE tied low) discard our own bytes first.
    size_t echo = BOARD.rs485_tx_echo ? datalen : 0;
    uint8_t scratch[64];
    while (echo) {
        int n = hal_serial_read(scratch, echo < sizeof(scratch) ? echo : sizeof(scratch), to);
        if (n <= 0)
            break;
        echo -= (size_t)n;
    }

    // Reply = 'S','Q','<' + raw slave bytes, so the configurator can tell a bridge
    // reply from periodic Modbus telemetry. Wait the response window for the first
    // byte, then drain until a short inter-byte gap marks the end of the frame.
    uint8_t reply[meshtastic_Constants_DATA_PAYLOAD_LEN];
    reply[0] = 'S';
    reply[1] = 'Q';
    reply[2] = replyMarker;   // '<' for the USB tool, '{' for the tunnel
    const size_t cap = sizeof(reply) - 3;
    size_t got = 0;
    int n = hal_serial_read(reply + 3, cap, to);
    if (n > 0) {
        got = (size_t)n;
        while (got < cap) {
            int m = hal_serial_read(reply + 3 + got, cap - got, 25);
            if (m <= 0)
                break;
            got += (size_t)m;
        }
    }

    char hx[2 * 16 + 1];
    size_t hn = got < 16 ? got : 16;
    for (size_t i = 0; i < hn; i++)
        snprintf(hx + 2 * i, 3, "%02x", reply[3 + i]);
    hx[2 * hn] = 0;
    LOG_INFO("ModbusModule: USB<->RS485 bridge @%u baud, tx %u -> rx %u bytes: %s",
             (unsigned)baud, (unsigned)datalen, (unsigned)got, hx);

    // Restore the poller's configured link params if the bridge changed them, so
    // the next periodic Modbus read still runs at the provisioned baud/parity.
    if (baud != g_cfg.modbus.baud || par != g_cfg.modbus.parity || stop != g_cfg.modbus.stop_bits)
        hal_serial_init(g_cfg.modbus.baud, g_cfg.modbus.parity, g_cfg.modbus.stop_bits);

    // Always reply (got == 0 ⇒ the UI shows "no reply").
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return;
    p->want_ack = false;
    memcpy(p->decoded.payload.bytes, reply, 3 + got);
    p->decoded.payload.size = 3 + got;
    if (local) {
        // Locally-driven converter: deliver ONLY to the USB/BLE client. sendToPhone()
        // queues straight to the local API with no RF transmit — no LoRa airtime.
        p->from = nodeDB->getNodeNum();
        service->sendToPhone(p);
    } else {
        // Remote-driven: unicast the reply back over the mesh to the requester, on the
        // channel the request came in on, so it reaches the originator's configurator.
        p->to = replyTo;
        p->channel = channel;
        service->sendToMesh(p, RX_SRC_LOCAL, true);   // + cc to a phone here, if any
    }
}

/* ── Transparent tunnel (master side) ──────────────────────────────────────────
   Reads raw bytes off the LOCAL port (RS485, or the USB CDC in the SQ_USB_TUNNEL
   build), frames them by idle gap (protocol-agnostic — works for non-Modbus devices
   too), and forwards each frame to the peer node. The peer answers via the raw-bridge
   path; the reply is written back to the local port by tunnelWriteback(). Non-blocking:
   polls the port in small windows so the main thread isn't starved. */
int32_t ModbusModule::tunnelPump()
{
    const size_t cap = meshtastic_Constants_DATA_PAYLOAD_LEN - 9;   // room after 'SQ}'+linkhdr
    uint8_t tmp[128];
    int n = tun_read(tmp, sizeof(tmp));
    uint32_t now = hal_millis();
    if (n > 0) {
        size_t room = (tunLen < cap) ? (cap - tunLen) : 0;
        size_t take = ((size_t)n < room) ? (size_t)n : room;
        memcpy(tunBuf + tunLen, tmp, take);
        tunLen += take;
        tunLastByte = now;
        if (tunLen >= cap)                  // frame at the size limit — flush now
            forwardTunnel();
        return 2;                           // more bytes likely still arriving
    }
    if (tunLen > 0 && (now - tunLastByte) >= g_cfg.tunnel.idle_gap_ms) {
        forwardTunnel();                    // idle gap on the bus → end of frame
        return 2;
    }
    return tunLen ? 2 : 10;                 // mid-frame: poll fast; idle: relax
}

void ModbusModule::forwardTunnel()
{
    if (tunLen == 0)
        return;
    meshtastic_MeshPacket *p = allocDataPacket();
    if (p) {
        uint8_t *d = p->decoded.payload.bytes;
#ifdef SQ_USB_TUNNEL
        // USB↔USB pipe: a symmetric one-way data push 'S','Q','~' + raw bytes. The peer
        // (also a USB-tunnel node) writes them straight to ITS USB — no RS485, no reply.
        d[0] = 'S'; d[1] = 'Q'; d[2] = '~';
        memcpy(d + 3, tunBuf, tunLen);
        p->decoded.payload.size = (pb_size_t)(3 + tunLen);
#else
        // RS485 tunnel: 'S','Q','}' + link header (baud u32 LE, parity u8, stop u8) + the
        // raw frame. The peer's raw-bridge runs it on RS485 and replies with marker '{'.
        const uint32_t baud = g_cfg.modbus.baud;
        d[0] = 'S'; d[1] = 'Q'; d[2] = '}';
        d[3] = baud & 0xff; d[4] = (baud >> 8) & 0xff; d[5] = (baud >> 16) & 0xff; d[6] = (baud >> 24) & 0xff;
        d[7] = (uint8_t)g_cfg.modbus.parity; d[8] = g_cfg.modbus.stop_bits;
        memcpy(d + 9, tunBuf, tunLen);
        p->decoded.payload.size = (pb_size_t)(9 + tunLen);
#endif
        p->want_ack = false;
        p->to = g_cfg.tunnel.peer_node;
        p->channel = g_cfg.tx.channel;
        service->sendToMesh(p, RX_SRC_LOCAL, false);   // unicast to the peer over the mesh
        LOG_INFO("ModbusModule: tunnel fwd %u bytes -> 0x%08x", (unsigned)tunLen,
                 (unsigned)g_cfg.tunnel.peer_node);
    }
    tunLen = 0;
}

void ModbusModule::tunnelWriteback(const uint8_t *data, size_t len)
{
    if (len == 0)
        return;
    tun_write(data, len);   // RS485 (DE + echo-discard) or plain USB CDC write
    tunLen = 0;             // drop any partial the framer accumulated during the writeback
    LOG_INFO("ModbusModule: tunnel writeback %u bytes -> local port", (unsigned)len);
}

void ModbusModule::applyBlePower(int dbm, bool persist)
{
    // This NimBLE/IDF build only exposes setPower(esp_power_level_t), so map the dBm
    // to the nearest 3 dB step. Regulatory note: BLE is 2.4 GHz — don't run at max
    // for production (NCC/FCC EIRP).
    esp_power_level_t lvl;
    switch (dbm) {
    case -12: lvl = ESP_PWR_LVL_N12; break;
    case -9:  lvl = ESP_PWR_LVL_N9;  break;
    case -6:  lvl = ESP_PWR_LVL_N6;  break;
    case -3:  lvl = ESP_PWR_LVL_N3;  break;
    case 3:   lvl = ESP_PWR_LVL_P3;  break;
    case 6:   lvl = ESP_PWR_LVL_P6;  break;
    case 9:   lvl = ESP_PWR_LVL_P9;  break;
    default:  lvl = ESP_PWR_LVL_N0; dbm = 0; break;   // 0 dBm
    }
    NimBLEDevice::setPower(lvl);
    if (persist) {
        int8_t v = (int8_t)dbm;
        hal_store_set("blepwr", &v, sizeof(v));
        hal_store_commit();
    }
    LOG_INFO("ModbusModule: BLE TX power %d dBm%s", dbm, persist ? " (saved)" : "");
}

#endif // SQC485IV2
