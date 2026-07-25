/*
 * test_sqcmd.c — the SQ command surface.
 *
 * Every packet on the private portnum starts 'S','Q', and the node has to sort
 * commands from config blobs from its OWN replies using nothing but those bytes.
 * Two failure modes matter and neither is visible in a code review:
 *
 *   - a marker byte that collides with a config-blob version, so a command gets
 *     fed to the blob parser and NAKed;
 *   - a reply that classifies as a command, so answering it produces another
 *     reply — a packet loop, on the air, at range.
 *
 * The classifier is pure, so both are cheap to pin here rather than on a mesh.
 */
#include <string.h>

#include "config.h"
#include "sq_test.h"
#include "sqcmd.h"

static sq_cmd_t classify_str(const char *s)
{
    return sq_classify((const uint8_t *)s, strlen(s));
}

/* ── the command set ──────────────────────────────────────────────────────── */

void test_sqcmd_recognises_every_command(void)
{
    ASSERT_EQ(SQ_CMD_POLL_NOW, classify_str("SQ?"));
    ASSERT_EQ(SQ_CMD_CAPABILITY, classify_str("SQV?"));
    ASSERT_EQ(SQ_CMD_GET_CONFIG, classify_str("SQG?"));
    ASSERT_EQ(SQ_CMD_RS485_BRIDGE, classify_str("SQ>"));
    ASSERT_EQ(SQ_CMD_TUNNEL_FORWARD, classify_str("SQ}"));
    ASSERT_EQ(SQ_CMD_TUNNEL_REPLY, classify_str("SQ{"));
    ASSERT_EQ(SQ_CMD_PIPE_DATA, classify_str("SQ~"));

    const uint8_t ble[4] = {'S', 'Q', 'P', 9}; /* +9 dBm */
    ASSERT_EQ(SQ_CMD_BLE_POWER, sq_classify(ble, sizeof(ble)));
}

/* The frame-bearing commands carry their payload immediately after the marker,
   and must work with an empty payload rather than reading off the end. */
void test_sqcmd_frame_commands_carry_a_payload(void)
{
    const uint8_t bridge[] = {'S', 'Q', '>', 0x01, 0x03, 0x00, 0x00};
    ASSERT_EQ(SQ_CMD_RS485_BRIDGE, sq_classify(bridge, sizeof(bridge)));
    ASSERT_EQ(4, sizeof(bridge) - SQ_FRAME_PAYLOAD_OFFSET);

    ASSERT_EQ(SQ_CMD_RS485_BRIDGE, classify_str("SQ>")); /* zero-length frame is legal */
}

/* The two bridge callers must never be answered with the same marker, or a
   manual USB probe of a peer gets mistaken for a tunnel reply and injected onto
   that peer's bus. */
void test_sqcmd_bridge_reply_markers_are_distinct(void)
{
    ASSERT_EQ('<', sq_bridge_reply_marker(SQ_CMD_RS485_BRIDGE));
    ASSERT_EQ('{', sq_bridge_reply_marker(SQ_CMD_TUNNEL_FORWARD));
    ASSERT_TRUE(sq_bridge_reply_marker(SQ_CMD_RS485_BRIDGE) != sq_bridge_reply_marker(SQ_CMD_TUNNEL_FORWARD));

    ASSERT_EQ(0, sq_bridge_reply_marker(SQ_CMD_POLL_NOW));
    ASSERT_EQ(0, sq_bridge_reply_marker(SQ_CMD_CONFIG_BLOB));
}

/* ── what must NOT be treated as a command ────────────────────────────────── */

void test_sqcmd_ignores_short_and_foreign_payloads(void)
{
    ASSERT_EQ(SQ_CMD_IGNORE, sq_classify((const uint8_t *)"", 0));
    ASSERT_EQ(SQ_CMD_IGNORE, classify_str("S"));
    ASSERT_EQ(SQ_CMD_IGNORE, classify_str("SQ"));
    ASSERT_EQ(SQ_CMD_IGNORE, classify_str("XQ?"));
    ASSERT_EQ(SQ_CMD_IGNORE, classify_str("SX?"));

    /* Raw-forward telemetry: [slave][func][data…]. Our own uplinks come back on
       this portnum and must not be parsed as anything. */
    const uint8_t telemetry[] = {0x01, 0x03, 0xAA, 0x55, 0x12, 0x34};
    ASSERT_EQ(SQ_CMD_IGNORE, sq_classify(telemetry, sizeof(telemetry)));
}

/* The four-byte commands need their fourth byte; three bytes is not enough to
   tell 'SQP' the command from a truncated frame. */
void test_sqcmd_four_byte_commands_need_four_bytes(void)
{
    ASSERT_EQ(SQ_CMD_IGNORE, classify_str("SQP"));
    ASSERT_EQ(SQ_CMD_IGNORE, classify_str("SQV"));
    ASSERT_EQ(SQ_CMD_IGNORE, classify_str("SQG"));
}

/* A query is a query only with its trailing '?'. Without it, the same bytes are
   the START of the reply to that query — which is why the marker exists. */
void test_sqcmd_queries_require_the_question_mark(void)
{
    ASSERT_EQ(SQ_CMD_CAPABILITY, classify_str("SQV?"));
    ASSERT_EQ(SQ_CMD_OWN_REPLY, classify_str("SQVx"));

    ASSERT_EQ(SQ_CMD_GET_CONFIG, classify_str("SQG?"));
    ASSERT_EQ(SQ_CMD_OWN_REPLY, classify_str("SQGx"));
}

/* THE LOOP GUARD. Every reply this firmware emits, fed back into the classifier,
   must come out as something it will not answer. */
void test_sqcmd_own_replies_are_never_commands(void)
{
    /* 'SQ!' config acknowledgement */
    uint8_t ack[SQ_CONFIG_ACK_LEN];
    ASSERT_EQ(SQ_CONFIG_ACK_LEN, sq_build_config_ack(0, ack, sizeof(ack)));
    ASSERT_EQ(SQ_CMD_OWN_REPLY, sq_classify(ack, SQ_CONFIG_ACK_LEN));

    /* 'SQV…' capability report */
    uint8_t cap[SQ_CAP_REPLY_MAX];
    size_t cap_len = sq_build_capability_reply(cap, sizeof(cap));
    ASSERT_TRUE(cap_len > 0);
    ASSERT_EQ(SQ_CMD_OWN_REPLY, sq_classify(cap, cap_len));

    /* 'SQG…' config read-back, whose 4th byte is the blob's 'S' magic */
    sq_config_t cfg;
    config_set_defaults(&cfg);
    uint8_t getcfg[200];
    size_t getcfg_len = sq_build_get_config_reply(&cfg, getcfg, sizeof(getcfg));
    ASSERT_TRUE(getcfg_len > 0);
    ASSERT_EQ(SQ_CMD_OWN_REPLY, sq_classify(getcfg, getcfg_len));

    /* 'SQ<' raw-bridge answer */
    const uint8_t bridge_reply[] = {'S', 'Q', '<', 0x01, 0x03, 0x04, 0xAA};
    ASSERT_EQ(SQ_CMD_OWN_REPLY, sq_classify(bridge_reply, sizeof(bridge_reply)));

    /* 'SQ{' is the exception: it IS a command (the peer's tunnel answer), so the
       module gates it on tunnel.enabled AND from == peer_node. Classification
       alone must not be what stops a node acting on its own '{' reply. */
    ASSERT_EQ(SQ_CMD_TUNNEL_REPLY, classify_str("SQ{"));
}

/* ── raw-bridge request body ──────────────────────────────────────────────── */

static sq_modbus_t link_from_defaults(void)
{
    sq_config_t c;
    config_set_defaults(&c);
    return c.modbus; /* 9600 8N1 */
}

void test_sqcmd_bridge_request_with_header(void)
{
    sq_modbus_t link = link_from_defaults();

    /* 19200, even parity, 2 stop bits, then an 8-byte Modbus frame */
    const uint8_t body[] = {0x00, 0x4B, 0x00, 0x00, 0x01, 0x02, 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B};
    sq_bridge_req_t req;
    sq_parse_bridge_request(body, sizeof(body), &link, &req);

    ASSERT_TRUE(req.had_header);
    ASSERT_EQ(19200, req.baud);
    ASSERT_EQ(1, req.parity);
    ASSERT_EQ(2, req.stop_bits);
    ASSERT_EQ(8, req.frame_len);
    ASSERT_MEM_EQ(body + SQ_BRIDGE_HEADER_LEN, req.frame, 8);
}

void test_sqcmd_bridge_request_without_header_uses_the_link_config(void)
{
    sq_modbus_t link = link_from_defaults();
    link.baud = 57600;
    link.stop_bits = 2;

    const uint8_t body[] = {0xDE, 0xAD, 0xBE};
    sq_bridge_req_t req;
    sq_parse_bridge_request(body, sizeof(body), &link, &req);

    ASSERT_FALSE(req.had_header);
    ASSERT_EQ(57600, req.baud);
    ASSERT_EQ(2, req.stop_bits);
    ASSERT_EQ(3, req.frame_len);
    ASSERT_MEM_EQ(body, req.frame, 3);
}

void test_sqcmd_bridge_stop_bits_zero_means_one(void)
{
    sq_modbus_t link = link_from_defaults();
    const uint8_t body[] = {0x80, 0x25, 0x00, 0x00, 0x00, 0x00, 0xAA};
    sq_bridge_req_t req;
    sq_parse_bridge_request(body, sizeof(body), &link, &req);

    ASSERT_EQ(9600, req.baud);
    ASSERT_EQ(1, req.stop_bits); /* 0 is not a legal frame, treat it as 1 */
}

/* THE TRAP, pinned deliberately.
 *
 * The header is positional and untagged, and the rule is "6 bytes or more means
 * there is a header". An 8-byte Modbus read request — the single most obvious
 * thing to hand a raw bridge — therefore parses as link parameters plus a 2-byte
 * frame. The device retunes its UART to 769 baud, transmits two stray bytes, and
 * LEAVES the UART there, so every subsequent poll fails as well.
 *
 * This cost real debugging time: the symptom is a silent bus that looks exactly
 * like mis-wiring. The format cannot change without breaking deployed
 * configurators, so the mitigation is that every caller sends the header and this
 * test states why.
 */
void test_sqcmd_bare_modbus_frame_is_misread_as_a_header(void)
{
    sq_modbus_t link = link_from_defaults();

    const uint8_t bare[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B};
    sq_bridge_req_t req;
    sq_parse_bridge_request(bare, sizeof(bare), &link, &req);

    ASSERT_TRUE(req.had_header);     /* it "found" a header that was never sent */
    ASSERT_EQ(0x00000301, req.baud); /* 769 baud, from the slave/function bytes */
    ASSERT_EQ(2, req.frame_len);     /* only the CRC reaches the wire */
    ASSERT_EQ(0xC4, req.frame[0]);
    ASSERT_EQ(0x0B, req.frame[1]);
}

/* ── config blobs ─────────────────────────────────────────────────────────── */

void test_sqcmd_accepts_every_supported_blob_version(void)
{
    for (uint8_t version = 2; version <= 4; version++) {
        uint8_t blob[64];
        memset(blob, 0, sizeof(blob));
        blob[0] = 'S';
        blob[1] = 'Q';
        blob[2] = version;
        /* Classification is byte-pattern only — config_from_blob() still has the
           final say on length and CRC. */
        ASSERT_EQ(SQ_CMD_CONFIG_BLOB, sq_classify(blob, sizeof(blob)));
    }
}

/* The version field and the marker bytes must not overlap: a version is 2..15,
   every marker is printable ASCII (>= 0x20). */
void test_sqcmd_version_range_cannot_collide_with_markers(void)
{
    const char markers[] = "?PVG><}{~!<";
    for (const char *m = markers; *m; m++)
        ASSERT_TRUE((uint8_t)*m > 15);

    uint8_t frame[8] = {'S', 'Q', 0, 0, 0, 0, 0, 0};
    for (int v = 2; v <= 15; v++) {
        frame[2] = (uint8_t)v;
        ASSERT_EQ(SQ_CMD_CONFIG_BLOB, sq_classify(frame, sizeof(frame)));
    }
    for (int v = 0; v <= 1; v++) {
        frame[2] = (uint8_t)v;
        ASSERT_EQ(SQ_CMD_IGNORE, sq_classify(frame, sizeof(frame))); /* unknown version */
    }
    for (int v = 16; v < 0x20; v++) {
        frame[2] = (uint8_t)v;
        ASSERT_EQ(SQ_CMD_IGNORE, sq_classify(frame, sizeof(frame))); /* not a version, not ASCII */
    }
}

/* ── reply builders ───────────────────────────────────────────────────────── */

/* Byte-for-byte against the layout the configurator parses. The expected bytes
   below are the ones a real SQC485Iv2 answers with. */
void test_sqcmd_capability_reply_layout(void)
{
    uint8_t r[SQ_CAP_REPLY_MAX];
    size_t n = sq_build_capability_reply(r, sizeof(r));

    ASSERT_TRUE(n > 0);
    ASSERT_EQ('S', r[0]);
    ASSERT_EQ('Q', r[1]);
    ASSERT_EQ('V', r[2]);
    ASSERT_EQ(SQ_CAP_PROTO, r[3]);
    ASSERT_EQ(SQ_CONFIG_VERSION, r[4]);
    ASSERT_EQ(SQ_FEATURES, r[5]);

    uint8_t fw_len = r[6];
    ASSERT_EQ(strlen(SQ_FW_VERSION), fw_len);
    ASSERT_MEM_EQ(SQ_FW_VERSION, r + 7, fw_len);

    /* product id is appended from proto 2 onwards */
    uint8_t pid_len = r[7 + fw_len];
    ASSERT_EQ(strlen(SQ_PRODUCT_ID), pid_len);
    ASSERT_MEM_EQ(SQ_PRODUCT_ID, r + 8 + fw_len, pid_len);

    ASSERT_EQ(8 + fw_len + pid_len, n);
}

void test_sqcmd_get_config_reply_wraps_the_blob(void)
{
    sq_config_t cfg;
    config_set_defaults(&cfg);

    uint8_t reply[200];
    size_t n = sq_build_get_config_reply(&cfg, reply, sizeof(reply));

    uint8_t blob[200];
    size_t blob_len = config_to_blob(&cfg, blob, sizeof(blob));

    ASSERT_EQ(3 + blob_len, n);
    ASSERT_MEM_EQ("SQG", reply, 3);
    ASSERT_MEM_EQ(blob, reply + 3, blob_len);
}

/* The mesh payload the module hands poll_collect_raw() (ModbusModule.cpp). */
#define MESH_PAYLOAD_LEN 233

void test_sqcmd_plan_payload_length(void)
{
    sq_config_t c;
    config_set_defaults(&c);

    /* the shipped default plan: 2 regs + 1 reg + 1 reg */
    ASSERT_EQ((2 + 4) + (2 + 2) + (2 + 2), sq_plan_payload_len(&c));
    ASSERT_TRUE(sq_plan_payload_len(&c) <= MESH_PAYLOAD_LEN);

    c.poll_count = 0;
    ASSERT_EQ(0, sq_plan_payload_len(&c));
}

/* The config format can express a plan the radio cannot carry, and the poll
   engine's response is to silently drop whole polls off the end. The device
   refuses such a plan instead, so the operator finds out at configuration time
   rather than by noticing two datapoints are permanently missing. */
void test_sqcmd_widest_plan_exceeds_a_mesh_payload(void)
{
    sq_config_t c;
    config_set_defaults(&c);
    c.poll_count = SQ_MAX_POLLS;
    for (int i = 0; i < SQ_MAX_POLLS; i++) {
        c.polls[i].slave = (uint8_t)(i + 1);
        c.polls[i].function = 3;
        c.polls[i].reg_count = SQ_MAX_REGS;
    }

    ASSERT_EQ(272, sq_plan_payload_len(&c));
    ASSERT_TRUE(sq_plan_payload_len(&c) > MESH_PAYLOAD_LEN);

    /* Six full-width polls is the real limit; the seventh tips it over. */
    c.poll_count = 6;
    ASSERT_EQ(204, sq_plan_payload_len(&c));
    ASSERT_TRUE(sq_plan_payload_len(&c) <= MESH_PAYLOAD_LEN);

    c.poll_count = 7;
    ASSERT_EQ(238, sq_plan_payload_len(&c));
    ASSERT_TRUE(sq_plan_payload_len(&c) > MESH_PAYLOAD_LEN);
}

void test_sqcmd_config_ack_carries_the_status(void)
{
    for (uint8_t status = 0; status <= 3; status++) {
        uint8_t ack[SQ_CONFIG_ACK_LEN];
        ASSERT_EQ(SQ_CONFIG_ACK_LEN, sq_build_config_ack(status, ack, sizeof(ack)));
        ASSERT_MEM_EQ("SQ!", ack, 3);
        ASSERT_EQ(status, ack[3]);
    }
}

/* A builder that cannot fit must write nothing and say so, rather than truncate
   a reply the configurator would then misparse. */
void test_sqcmd_builders_reject_small_buffers(void)
{
    uint8_t small[4];
    ASSERT_EQ(0, sq_build_capability_reply(small, sizeof(small)));
    ASSERT_EQ(0, sq_build_config_ack(0, small, 3));

    sq_config_t cfg;
    config_set_defaults(&cfg);
    ASSERT_EQ(0, sq_build_get_config_reply(&cfg, small, sizeof(small)));
    ASSERT_EQ(0, sq_build_get_config_reply(&cfg, small, 2));
}
