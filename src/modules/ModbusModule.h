/*
 * ModbusModule.h — Siliqs config-driven Modbus-over-mesh module for Meshtastic.
 *
 * The generic ("forward firmware") Mesh version. This is the de-branded
 * generalization of 16_'s PLCModbusModule: instead of hardcoded FC03 / slave 1 /
 * 15 s, the poll plan + link params come from the SHARED firmware_core config
 * (binary blob in our own LittleFS file), and the Modbus + raw-forward payload is
 * the SAME portable engine the Star (LoRaWAN) version uses:
 *     firmware_core/src/{modbus,poll,config}.c   (backed by hal_meshtastic.cpp)
 * So one engine, two radios — differentiation is config, not a code fork.
 *
 * Drops into a Meshtastic fork (needs SinglePortModule / OSThread / MeshService /
 * protobufs). API names follow Meshtastic master as surveyed 2026-06; VERIFY
 * against the pinned tag before building — the engine drifts ~monthly.
 * Mirrors the official EnvironmentTelemetry module shape:
 *   SinglePortModule (raw bytes on our private PortNum) + OSThread (periodic poll).
 *
 * Build guard: compiled only when -DSQC485IV2 is set (the board variant), like
 * the 16_ module, so a stock Meshtastic build is unaffected.
 */
#pragma once

#include "mesh/SinglePortModule.h"
#include "concurrency/OSThread.h"

// Private application PortNum for Siliqs Modbus payloads. Must be in the
// 256–511 "private / experimental" range; fixed so the cloud decoder matches it.
// Same value the 16_ deployment uses (meshtastic_PortNum_PRIVATE_APP == 256).
#define SILIQS_MODBUS_PORTNUM ((meshtastic_PortNum)256)

class ModbusModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    ModbusModule();

  protected:
    // Periodic poll → raw-forward payload → mesh-send. Returns ms until next call.
    virtual int32_t runOnce() override;

    // Accept config-blob downlinks addressed to our PortNum (companion-app
    // provisioning over the mesh). Telemetry vs config is told apart by the
    // 'SQ' blob magic + CRC, which config_from_blob() validates.
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

    // Also see ROUTING_APP packets so a Level-3 (#10) confirmed uplink can catch the
    // ACK addressed back to us (request_id == our sent id). Our PortNum otherwise.
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override
    {
        return p->decoded.portnum == ourPortNum ||
               p->decoded.portnum == meshtastic_PortNum_ROUTING_APP;
    }

  private:
    bool firstTime = true;
    void pollAndSend();          // uses the shared firmware_core engine
    void applyConfigBlob(const uint8_t *blob, size_t len, uint32_t from);

    // Epic G duty-cycle deep sleep for a "mute sensor" leaf. After the uplink is
    // enqueued, runOnce() arms the sleep, waits for the send to settle, then powers
    // the SX126x + MCU down for the interval (reboots on wake).
    //   Level 4 (#9): broadcast, fire-and-forget → sleep once the radio TX drains.
    //   Level 3 (#10): unicast want_ack to dest_node → sleep once the ACK lands (or
    //                  the reliable-delivery retransmissions are exhausted).
    int      sleepMode();        // 0 = stay awake, 3 = confirmed sleep, 4 = fire-and-forget sleep
    void     enterDeepSleep();   // sleep radio + esp_deep_sleep for the interval (no return)
    bool     sleepArmed = false; // uplink sent last cycle; waiting to settle before sleep
    uint32_t sleepArmedAt = 0;   // hal_millis() when armed (drain / ACK-wait window)
    uint32_t ackWaitId = 0;      // L3: id of the want_ack uplink we're waiting to be ACKed (0 = none)
    bool     ackReceived = false;// L3: a routing ACK/NAK for ackWaitId came back

    // Keep a deep-sleep leaf awake while the configurator (USB/BLE) is attached, so it
    // can be reconfigured; it resumes sleeping after you disconnect. Plus a post-reset
    // grace window to give a configurator time to connect before the first sleep.
    bool     clientConnected();  // a client is attached over the API (service->api_state)
    bool     bootChecked = false;// one-shot: set the connect grace based on wake cause
    uint32_t connectGraceUntil = 0;   // hal_millis() until which we defer sleep (post-reset)
    bool     sleepSuppressedLogged = false;  // log "deferred" once per attach, not every cycle

    // Capability/ACK replies ('SQ V' report, 'SQ !' config apply status): to the local
    // USB/BLE client when from==self, else unicast back to the requester over the mesh.
    void sendSqReply(const uint8_t *data, size_t len, uint32_t from, uint8_t channel);

    // RS485↔RS485 transparent tunnel (master side). tunnelPump() reads the local bus
    // and forwardTunnel() ships each idle-gap-framed frame to the peer; the peer's
    // reply is written back to the local bus by tunnelWriteback().
    int32_t tunnelPump();
    void    forwardTunnel();
    void    tunnelWriteback(const uint8_t *data, size_t len);
    uint8_t  tunBuf[233];        // accumulating frame from the local RS485
    size_t   tunLen = 0;
    uint32_t tunLastByte = 0;    // hal_millis() of the last byte (idle-gap framing)

    // BLE TX power (set via the 'SQ P' command; persisted to a separate store key and
    // re-applied once at startup).
    bool blePowerApplied = false;
    void applyBlePower(int dbm, bool persist);

    // USB↔RS485 transparent bridge: write the caller's raw bytes to the RS485 line
    // and return whatever the slave replies. local=true → reply to the USB client
    // only (no RF); local=false → unicast the reply back over the mesh to replyTo on
    // the given channel (so a remote node can be driven from another node's USB).
    // replyMarker is the 3rd reply byte ('<' for the USB tool, '{' for the tunnel) so
    // the two paths never cross even when they target the same peer.
    void rawBridge(const uint8_t *req, size_t reqlen, uint32_t replyTo, uint8_t channel,
                   bool local, uint8_t replyMarker);
};

extern ModbusModule *modbusModule;
