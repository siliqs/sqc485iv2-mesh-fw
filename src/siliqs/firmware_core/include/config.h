/* config.h — customer-facing config contract. Raw-forward payload model:
   the device forwards raw Modbus bytes; per-poll type/scale/name are CLOUD
   metadata (used by the cloud decoder, NOT sent over the air). type defaults to
   SQ_TYPE_RAW = transparent passthrough (opaque bytes, decoded in cloud). */
#pragma once
#include "hal/hal_serial.h" /* hal_parity_t */
#include <stdbool.h>
#include <stdint.h>

#define SQ_CONFIG_VERSION                                                                                                        \
    5 /* v5: + gateway Modbus-slave reply (transparent + aggregated,                                                             \
          staleness threshold, register map); v2/v3/v4 still accepted */

/* Capability handshake (configurator ⇄ node). The configurator sends 'S','Q','V','?'
   and the node replies 'S','Q','V', proto, max_blob_ver, features, fw_len, fw[…],
   pid_len, product_id[…] so the UI can show the real product + firmware and gate/verify
   features. Bump SQ_FW_VERSION on every firmware release; bump SQ_CAP_PROTO only if this
   reply format itself changes. proto 2 = product_id appended after fw. */
#define SQ_CAP_PROTO 2
#define SQ_FW_VERSION "1.4.2"
/* Which Siliqs product this board is — Meshtastic's HardwareModel has no per-vendor value
   (custom boards all report PRIVATE_HW), so the configurator reads this to label the node. */
#if defined(SQC485IV2)
#define SQ_PRODUCT_ID "SQC485Iv2"
#elif defined(SQS_TH_I)
#define SQ_PRODUCT_ID "SQS-TH-I"
#else
#define SQ_PRODUCT_ID "SQC485I"
#endif
#define SQ_FEAT_TUNNEL 0x01        /* RS485↔RS485 tunnel (blob v4)      */
#define SQ_FEAT_BLE_POWER 0x02     /* 'SQ P' live BLE TX power          */
#define SQ_FEAT_RS485_TERM 0x04    /* 'SQ>' USB/remote RS485 terminal   */
#define SQ_FEAT_POLL_NOW 0x08      /* 'SQ?' poll-now                    */
#define SQ_FEAT_DEEP_SLEEP 0x10    /* Epic G L4: duty-cycle deep sleep (mute leaf, #9) */
#define SQ_FEAT_GET_CONFIG 0x20    /* 'SQG?' read-back current config blob             */
#define SQ_FEAT_SENSOR 0x40        /* built-in-sensor node (SQS-*): no external RS485   */
#define SQ_FEAT_GATEWAY_REPLY 0x80 /* gateway Modbus-slave reply, transparent + aggregated (blob v5) */
/* Feature set the node advertises in the 'SQV?' reply. The SQS-Sensor line has no
   RS485/tunnel/BLE-power features — just the sensor-node handshake — so the
   configurator can hide the Modbus UI and show only the report interval. */
#ifdef SQS_TH_I
#define SQ_FEATURES (SQ_FEAT_POLL_NOW | SQ_FEAT_DEEP_SLEEP | SQ_FEAT_GET_CONFIG | SQ_FEAT_SENSOR)
#else
#define SQ_FEATURES                                                                                                              \
    (SQ_FEAT_TUNNEL | SQ_FEAT_BLE_POWER | SQ_FEAT_RS485_TERM | SQ_FEAT_POLL_NOW | SQ_FEAT_DEEP_SLEEP | SQ_FEAT_GET_CONFIG |      \
     SQ_FEAT_GATEWAY_REPLY)
#endif

#define SQ_MAX_POLLS 8
#define SQ_MAX_REGS 16 /* registers per poll */
#define SQ_NAME_LEN 24
#define SQ_MAX_GW_MAP_ENTRIES 16 /* aggregated-mode register-map rows */

/* uplink_interval_s bounds, enforced by config_from_blob() so both products share
   one gate instead of each module clamping (or not) on its own. ModbusModule has
   no clamp at all today: an interval of 0 makes its runOnce() return 0 forever —
   a busy-loop that floods the mesh with zero backoff. That is the actual danger,
   so the floor only excludes 0 — test/hil/qc_checks.py deliberately uses 4s
   intervals (CFG-07, MODE-01) to keep the QC gate's periodic-behaviour checks
   fast, and a first attempt at this floor (5) broke both of those on real
   hardware (2026-07-31). The upper bound stays well under the point where
   `(int32_t)uplink_interval_s * 1000` (used by every module that converts this
   to milliseconds) overflows, at ~2,147,483 s (~24.85 days). */
#define SQ_MIN_UPLINK_INTERVAL_S 1
#define SQ_MAX_UPLINK_INTERVAL_S 604800 /* 7 days */

/* Cloud-decode hint for a datapoint. RAW (=0, default) = passthrough: the cloud
   forwards the opaque bytes and does not interpret them. The rest tell the cloud
   how to slice/scale the raw bytes into a value. None of this is sent on air. */
typedef enum {
    SQ_TYPE_RAW = 0, /* passthrough — default */
    SQ_TYPE_U16,
    SQ_TYPE_I16,
    SQ_TYPE_U32,
    SQ_TYPE_I32,
    SQ_TYPE_F32
} sq_datatype_t;

typedef enum { SQ_WORD_AB, SQ_WORD_BA } sq_word_order_t; /* 32-bit word order */

typedef struct {
    char name[SQ_NAME_LEN]; /* cloud metadata (label) */
    uint8_t slave;          /* Modbus slave address */
    uint8_t function;       /* 3 (holding) or 4 (input) */
    uint16_t reg_start;
    uint16_t reg_count;
    /* cloud-decode metadata — NOT transmitted (raw-forward sends bytes regardless): */
    sq_datatype_t type; /* SQ_TYPE_RAW = passthrough */
    sq_word_order_t word_order;
    float scale;
    float offset;
} sq_poll_t;

typedef struct {
    uint8_t dev_eui[8], join_eui[8], app_key[16]; /* OTAA, AS923 (Star only) */
    bool adr;
    uint8_t dr;
    bool confirmed;
    uint8_t nb_trans;
} sq_lorawan_t;

typedef struct {
    uint32_t baud;
    hal_parity_t parity;
    uint8_t stop_bits;
    uint16_t response_timeout_ms;
    uint8_t retries;      /* per-poll read retries before the error frame */
    uint16_t poll_gap_ms; /* settle delay between consecutive polls */
} sq_modbus_t;

typedef struct {
    uint32_t uplink_interval_s;
    bool deep_sleep;
} sq_power_t;

/* Where the Mesh node sends its raw-forward telemetry (Mesh version only). */
typedef struct {
    uint32_t dest_node; /* 0 = broadcast; else unicast to this node num */
    uint8_t channel;    /* mesh channel index (0 = primary) */
    bool confirmed;     /* Mesh: want_ack unicast to dest_node (Epic G L3, #10). Only
                           meaningful when dest_node != 0 (can't ack a broadcast). */
} sq_tx_t;

/* RS485↔RS485 transparent tunnel (Mesh version only). When enabled the node is a
   "tunnel master": it reads raw frames off its LOCAL RS485 (idle-gap framed, any
   protocol — not just Modbus), forwards them to peer_node over the mesh, and
   writes the peer's reply back out the local RS485. The peer answers via the
   existing raw-bridge path. Replaces periodic polling while enabled. */
typedef struct {
    bool enabled;
    uint32_t peer_node;   /* the remote node whose RS485 we tunnel to/from */
    uint16_t idle_gap_ms; /* silence on the local bus that marks end-of-frame */
} sq_tunnel_t;

/* One row of the gateway's aggregated register map: where a remote node's cached
   telemetry (node_id, src_slave, src_func, src_reg_start .. +src_reg_count) lands
   in the gateway's own virtual register space (virt_reg_offset) when answered under
   aggregated_slave_addr. Unused in transparent mode. */
typedef struct {
    uint32_t node_id;
    uint8_t src_slave;
    uint8_t src_func; /* 3 (holding) or 4 (input) */
    uint16_t src_reg_start;
    uint16_t src_reg_count;
    uint16_t virt_reg_offset;
} sq_gw_map_entry_t;

/* Gateway role only (Mesh version): answer an external Modbus RTU master on the
   node's own RS485 bus from cached mesh telemetry — never by querying the mesh
   live (mesh round-trip is seconds to tens of seconds, far past any Modbus
   master's timeout). transparent and aggregated may both be enabled at once,
   each under its own slave address. A cache entry older than
   staleness_threshold_s replies with Modbus exception 0x0B (GATEWAY TARGET
   DEVICE FAILED TO RESPOND) instead of stale data. */
typedef struct {
    bool transparent_enabled; /* answer under each remote node's own slave address —
                                  requires slave addresses to be unique mesh-wide */
    bool aggregated_enabled;
    uint8_t aggregated_slave_addr;
    uint32_t staleness_threshold_s;
    uint8_t map_count;
    sq_gw_map_entry_t map[SQ_MAX_GW_MAP_ENTRIES];
} sq_gateway_reply_t;

typedef struct {
    uint16_t schema_version;
    char device_name[SQ_NAME_LEN];
    sq_lorawan_t lorawan;
    sq_modbus_t modbus;
    uint8_t poll_count;
    sq_poll_t polls[SQ_MAX_POLLS]; /* the configurable poll list */
    sq_power_t power;
    sq_tx_t tx;                 /* telemetry destination (Mesh) */
    bool rs485_enabled;         /* false = don't poll RS485 (no sensor wired) */
    sq_tunnel_t tunnel;         /* RS485↔RS485 transparent tunnel (Mesh) */
    sq_gateway_reply_t gateway; /* gateway Modbus-slave reply (Mesh, v5) */
} sq_config_t;

/* blob flags byte (offset 3) */
#define SQ_FLAG_RS485_OFF 0x01
#define SQ_FLAG_TUNNEL_ON 0x02
#define SQ_FLAG_CONFIRMED 0x04 /* Mesh: confirmed (want_ack) unicast uplink (L3, #10) */

/* gateway block gw_flags byte (v5, own byte — not the offset-3 flags above) */
#define SQ_FLAG_GW_TRANSPARENT 0x01
#define SQ_FLAG_GW_AGGREGATED 0x02

/* The gateway slave responder's "stale" reply is SQ_MB_EXC_GATEWAY_NO_RESPONSE
   (0x0B, modbus.h) — reuse that constant, don't redefine it here. "Not found"
   is SQ_MB_EXC_ILLEGAL_ADDRESS (0x02, modbus.h). */

void config_set_defaults(sq_config_t *c);
bool config_load(sq_config_t *c);
bool config_save(const sq_config_t *c);
bool config_factory_reset(sq_config_t *c);

/* Binary config blob — the device "download" path (no text parsing).
   Layout (little-endian): 'S','Q', ver, flags, name[16], baud(4), parity(1),
   stop(1), resp_timeout(2), retries(1), poll_gap(2), interval_s(4), deep_sleep(1),
   poll_count(1), polls[count]{slave(1) func(1) reg_start(2) reg_count(2)},
   [v3+] tx routing{dest_node(4) channel(1)},
   [v4+] tunnel{peer_node(4) idle_gap_ms(2)},
   [v5+] gateway{gw_flags(1) aggregated_slave_addr(1) staleness_threshold_s(4)
                 map_count(1) map[count]{node_id(4) src_slave(1) src_func(1)
                 src_reg_start(2) src_reg_count(2) virt_reg_offset(2)}},
   crc16(2).
   Only the read-plan goes to the device; per-poll type/scale are cloud-side.
   config_from_blob PRESERVES c->lorawan (keys are provisioned separately). */
size_t config_to_blob(const sq_config_t *c, uint8_t *out, size_t cap); /* len, 0 on fail */
bool config_from_blob(sq_config_t *c, const uint8_t *blob, size_t len);

/* type <-> name helpers (used by provisioning + logging). */
const char *sq_type_name(sq_datatype_t t);
bool sq_type_parse(const char *s, sq_datatype_t *out);
