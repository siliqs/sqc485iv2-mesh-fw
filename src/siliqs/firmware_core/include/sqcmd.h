/* sqcmd.h — the SQ command surface, as pure functions.
 *
 * Everything on the private portnum starts with the two bytes 'S','Q'. What
 * follows is either a command, one of our own replies, or a config blob, and
 * telling them apart is fiddlier than it looks: the markers must not collide
 * with a blob version byte, and the node must never act on its own reply (that
 * is a packet loop, and it happens over the air).
 *
 * That decision is byte-pattern only — no radio, no config, no node state — so
 * it lives here rather than inside the Meshtastic module, where it could only be
 * exercised on real hardware. The module keeps the parts that genuinely need
 * state: whether the tunnel is enabled, who the peer is, who to reply to.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef enum {
    SQ_CMD_IGNORE = 0,     /* not ours, too short, or a marker we do not act on */
    SQ_CMD_POLL_NOW,       /* "SQ?"        — read the plan now and uplink       */
    SQ_CMD_BLE_POWER,      /* "SQP" + int8 — set BLE TX power, dBm at byte 3    */
    SQ_CMD_CAPABILITY,     /* "SQV?"       — reply with the capability report   */
    SQ_CMD_GET_CONFIG,     /* "SQG?"       — reply with the current config blob */
    SQ_CMD_RS485_BRIDGE,   /* "SQ>" + raw  — run these bytes on RS485           */
    SQ_CMD_TUNNEL_FORWARD, /* "SQ}" + raw  — same, but from the tunnel master   */
    SQ_CMD_TUNNEL_REPLY,   /* "SQ{" + raw  — the peer's answer, write it locally */
    SQ_CMD_PIPE_DATA,      /* "SQ~" + raw  — USB<->USB pipe payload             */
    SQ_CMD_CONFIG_BLOB,    /* "SQ" + version 2..15 + ... — a config download    */
    SQ_CMD_OWN_REPLY       /* "SQ" + a marker byte: one of our own replies      */
} sq_cmd_t;

/* Classify one payload. Never inspects anything but the bytes. */
sq_cmd_t sq_classify(const uint8_t *b, size_t n);

/* Offset of the raw bytes carried by the frame-bearing commands (RS485_BRIDGE,
   TUNNEL_FORWARD, TUNNEL_REPLY, PIPE_DATA). */
#define SQ_FRAME_PAYLOAD_OFFSET 3

/* Reply marker a bridge request must be answered with, so the USB tool's traffic
   and the autonomous tunnel never cross even when they target the same peer.
   Returns 0 for anything that is not a bridge request. */
uint8_t sq_bridge_reply_marker(sq_cmd_t cmd);

/* ── raw-bridge request body ──────────────────────────────────────────────── */
/* A bridge request carries its own link parameters so the converter works even
   when the poller is disabled: 6 bytes of header, then the bytes to transmit.
 *
 *      baud (u32 LE) | parity (u8) | stop_bits (u8) | frame…
 *
 * ⚠ The header is positional, not tagged, and the rule is "6 or more bytes means
 * there is a header". An 8-byte Modbus frame sent without one is therefore not
 * rejected — it is silently read as baud/parity/stop plus a 2-byte frame, and
 * the device retunes its UART to the nonsense baud and leaves it there, so every
 * later poll fails too. Callers must always send the header; see
 * test_sqcmd_bare_modbus_frame_is_misread_as_a_header. */
#define SQ_BRIDGE_HEADER_LEN 6

typedef struct {
    uint32_t baud;
    uint8_t parity;
    uint8_t stop_bits;
    const uint8_t *frame; /* bytes to put on the wire */
    size_t frame_len;
    bool had_header; /* false = too short for a header, link params came from cfg */
} sq_bridge_req_t;

/* Split a bridge request body (the bytes after 'S','Q','>'). Link parameters
   fall back to *link when the body is shorter than the header. */
void sq_parse_bridge_request(const uint8_t *body, size_t n, const sq_modbus_t *link, sq_bridge_req_t *out);

/* ── reply builders ───────────────────────────────────────────────────────── */
/* Each returns the number of bytes written, or 0 if the buffer is too small. */

/* 'S','Q','V', proto, max_blob_ver, features, fw_len, fw[…], pid_len, pid[…] */
#define SQ_CAP_REPLY_MAX (9 + sizeof(SQ_FW_VERSION) + sizeof(SQ_PRODUCT_ID))
size_t sq_build_capability_reply(uint8_t *out, size_t cap);

/* 'S','Q','G' + the current config blob */
size_t sq_build_get_config_reply(const sq_config_t *cfg, uint8_t *out, size_t cap);

/* 'S','Q','!', status — 0 applied, 1 invalid, 2 valid but not persisted */
#define SQ_CONFIG_ACK_LEN 4
size_t sq_build_config_ack(uint8_t status, uint8_t *out, size_t cap);
