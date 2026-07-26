/* config.h — customer-facing config contract. Raw-forward payload model:
   the device forwards raw Modbus bytes; per-poll type/scale/name are CLOUD
   metadata (used by the cloud decoder, NOT sent over the air). type defaults to
   SQ_TYPE_RAW = transparent passthrough (opaque bytes, decoded in cloud). */
#pragma once
#include "hal/hal_serial.h" /* hal_parity_t */
#include <stdbool.h>
#include <stdint.h>

#define SQ_CONFIG_VERSION 4 /* v4: + RS485↔RS485 tunnel (peer + idle gap); v2/v3 still accepted */

/* Capability handshake (configurator ⇄ node). The configurator sends 'S','Q','V','?'
   and the node replies 'S','Q','V', proto, max_blob_ver, features, fw_len, fw[…],
   pid_len, product_id[…] so the UI can show the real product + firmware and gate/verify
   features. Bump SQ_FW_VERSION on every firmware release; bump SQ_CAP_PROTO only if this
   reply format itself changes. proto 2 = product_id appended after fw. */
#define SQ_CAP_PROTO 2
#define SQ_FW_VERSION "1.3.3"
/* Which Siliqs product this board is — Meshtastic's HardwareModel has no per-vendor value
   (custom boards all report PRIVATE_HW), so the configurator reads this to label the node. */
#if defined(SQC485IV2)
#define SQ_PRODUCT_ID "SQC485Iv2"
#else
#define SQ_PRODUCT_ID "SQC485I"
#endif
#define SQ_FEAT_TUNNEL 0x01     /* RS485↔RS485 tunnel (blob v4)      */
#define SQ_FEAT_BLE_POWER 0x02  /* 'SQ P' live BLE TX power          */
#define SQ_FEAT_RS485_TERM 0x04 /* 'SQ>' USB/remote RS485 terminal   */
#define SQ_FEAT_POLL_NOW 0x08   /* 'SQ?' poll-now                    */
#define SQ_FEAT_DEEP_SLEEP 0x10 /* Epic G L4: duty-cycle deep sleep (mute leaf, #9) */
#define SQ_FEAT_GET_CONFIG 0x20 /* 'SQG?' read-back current config blob             */
#define SQ_FEATURES                                                                                                              \
    (SQ_FEAT_TUNNEL | SQ_FEAT_BLE_POWER | SQ_FEAT_RS485_TERM | SQ_FEAT_POLL_NOW | SQ_FEAT_DEEP_SLEEP | SQ_FEAT_GET_CONFIG)

#define SQ_MAX_POLLS 8
#define SQ_MAX_REGS 16 /* registers per poll */
#define SQ_NAME_LEN 24

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

typedef struct {
    uint16_t schema_version;
    char device_name[SQ_NAME_LEN];
    sq_lorawan_t lorawan;
    sq_modbus_t modbus;
    uint8_t poll_count;
    sq_poll_t polls[SQ_MAX_POLLS]; /* the configurable poll list */
    sq_power_t power;
    sq_tx_t tx;         /* telemetry destination (Mesh) */
    bool rs485_enabled; /* false = don't poll RS485 (no sensor wired) */
    sq_tunnel_t tunnel; /* RS485↔RS485 transparent tunnel (Mesh) */
} sq_config_t;

/* blob flags byte (offset 3) */
#define SQ_FLAG_RS485_OFF 0x01
#define SQ_FLAG_TUNNEL_ON 0x02
#define SQ_FLAG_CONFIRMED 0x04 /* Mesh: confirmed (want_ack) unicast uplink (L3, #10) */

void config_set_defaults(sq_config_t *c);
bool config_load(sq_config_t *c);
bool config_save(const sq_config_t *c);
bool config_factory_reset(sq_config_t *c);

/* Binary config blob — the device "download" path (no text parsing).
   Layout (little-endian): 'S','Q', ver, flags, name[16], baud(4), parity(1),
   stop(1), resp_timeout(2), retries(1), poll_gap(2), interval_s(4), deep_sleep(1),
   poll_count(1), polls[count]{slave(1) func(1) reg_start(2) reg_count(2)}, crc16(2).
   Only the read-plan goes to the device; per-poll type/scale are cloud-side.
   config_from_blob PRESERVES c->lorawan (keys are provisioned separately). */
size_t config_to_blob(const sq_config_t *c, uint8_t *out, size_t cap); /* len, 0 on fail */
bool config_from_blob(sq_config_t *c, const uint8_t *blob, size_t len);

/* type <-> name helpers (used by provisioning + logging). */
const char *sq_type_name(sq_datatype_t t);
bool sq_type_parse(const char *s, sq_datatype_t *out);
