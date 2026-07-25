/*
 * test_blob.c — the config blob wire format.
 *
 * This is the device's public contract with the configurator, and it is
 * versioned in the field: a v2 blob from an old configurator must still be
 * accepted by today's firmware. Every fixture in golden/ was produced by an
 * INDEPENDENT implementation of the spec (golden/make_golden.py), so these tests
 * check the firmware against the documented format rather than against itself.
 */
#include <string.h>

#include "config.h"
#include "golden.h"
#include "mock_hal.h"
#include "modbus.h"
#include "sq_test.h"

#define BLOB_CAP 256

/* Distinctive LoRaWAN credentials, so "the blob left them alone" is provable. */
static void plant_lorawan(sq_config_t *c)
{
    for (int i = 0; i < 8; i++) {
        c->lorawan.dev_eui[i] = (uint8_t)(0xA0 + i);
        c->lorawan.join_eui[i] = (uint8_t)(0xB0 + i);
    }
    for (int i = 0; i < 16; i++)
        c->lorawan.app_key[i] = (uint8_t)(0xC0 + i);
    c->lorawan.dr = 4;
    c->lorawan.nb_trans = 3;
}

/* ── encoding ─────────────────────────────────────────────────────────────── */

/* The encoder is pinned byte-for-byte against the spec implementation. This is
   what catches a field moving, a width changing, or endianness flipping. */
void test_blob_encodes_defaults_byte_for_byte(void)
{
    uint8_t expect[BLOB_CAP];
    size_t expect_len = golden_load("v4_defaults", expect, sizeof(expect));
    ASSERT_EQ(68, expect_len);

    sq_config_t c;
    config_set_defaults(&c);

    uint8_t actual[BLOB_CAP];
    size_t n = config_to_blob(&c, actual, sizeof(actual));

    ASSERT_EQ(expect_len, n);
    ASSERT_MEM_EQ(expect, actual, expect_len);
}

void test_blob_encode_rejects_undersized_buffer(void)
{
    sq_config_t c;
    config_set_defaults(&c);

    uint8_t buf[BLOB_CAP];
    ASSERT_EQ(0, config_to_blob(&c, buf, 67)); /* needs 68 */
    ASSERT_EQ(0, config_to_blob(&c, buf, 0));
    ASSERT_EQ(68, config_to_blob(&c, buf, 68)); /* exact fit is fine */
}

void test_blob_encode_truncates_long_device_name(void)
{
    sq_config_t c;
    config_set_defaults(&c);
    strncpy(c.device_name, "ABCDEFGHIJKLMNOPQRST", SQ_NAME_LEN - 1); /* 20 chars */

    uint8_t blob[BLOB_CAP];
    size_t n = config_to_blob(&c, blob, sizeof(blob));
    ASSERT_TRUE(n > 0);
    ASSERT_MEM_EQ("ABCDEFGHIJKLMNOP", blob + 4, 16); /* the name field is 16 bytes */

    sq_config_t back;
    config_set_defaults(&back);
    ASSERT_TRUE(config_from_blob(&back, blob, n));
    ASSERT_STR_EQ("ABCDEFGHIJKLMNOP", back.device_name);
}

void test_blob_flag_bits(void)
{
    struct {
        bool rs485_enabled;
        bool tunnel;
        bool confirmed;
        uint8_t expect;
    } cases[] = {
        {true, false, false, 0x00}, {false, false, false, 0x01}, {true, true, false, 0x02},
        {true, false, true, 0x04},  {false, true, true, 0x07},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sq_config_t c;
        config_set_defaults(&c);
        c.rs485_enabled = cases[i].rs485_enabled;
        c.tunnel.enabled = cases[i].tunnel;
        c.tx.confirmed = cases[i].confirmed;

        uint8_t blob[BLOB_CAP];
        size_t n = config_to_blob(&c, blob, sizeof(blob));
        ASSERT_TRUE(n > 0);
        ASSERT_EQ(cases[i].expect, blob[3]);

        sq_config_t back;
        config_set_defaults(&back);
        ASSERT_TRUE(config_from_blob(&back, blob, n));
        ASSERT_EQ(cases[i].rs485_enabled, back.rs485_enabled);
        ASSERT_EQ(cases[i].tunnel, back.tunnel.enabled);
        ASSERT_EQ(cases[i].confirmed, back.tx.confirmed);
    }
}

/* ── decoding: version compatibility ──────────────────────────────────────── */

/* A v2 blob predates both tx routing and the tunnel. Those fields must land on
   documented defaults (broadcast, primary channel, tunnel off) rather than on
   whatever was in the struct. */
void test_blob_decode_v2_legacy(void)
{
    uint8_t blob[BLOB_CAP];
    size_t n = golden_load("v2", blob, sizeof(blob));
    ASSERT_EQ(45, n);

    sq_config_t c;
    config_set_defaults(&c);
    c.tx.dest_node = 0x12345678; /* must be overwritten with the v2 default */
    c.tx.channel = 5;
    c.tunnel.enabled = true;
    plant_lorawan(&c);

    ASSERT_TRUE(config_from_blob(&c, blob, n));

    ASSERT_STR_EQ("V2NODE", c.device_name);
    ASSERT_FALSE(c.rs485_enabled); /* flags bit0 */
    ASSERT_EQ(19200, c.modbus.baud);
    ASSERT_EQ(HAL_PARITY_ODD, c.modbus.parity);
    ASSERT_EQ(2, c.modbus.stop_bits);
    ASSERT_EQ(500, c.modbus.response_timeout_ms);
    ASSERT_EQ(5, c.modbus.retries);
    ASSERT_EQ(35, c.modbus.poll_gap_ms);
    ASSERT_EQ(300, c.power.uplink_interval_s);
    ASSERT_FALSE(c.power.deep_sleep);

    ASSERT_EQ(1, c.poll_count);
    ASSERT_EQ(7, c.polls[0].slave);
    ASSERT_EQ(4, c.polls[0].function);
    ASSERT_EQ(100, c.polls[0].reg_start);
    ASSERT_EQ(10, c.polls[0].reg_count);
    ASSERT_EQ(SQ_TYPE_RAW, c.polls[0].type); /* read plan only; type is cloud-side */
    ASSERT_FLOAT_EQ(1.0f, c.polls[0].scale);

    /* absent in v2 -> documented fallbacks */
    ASSERT_EQ(0, c.tx.dest_node);
    ASSERT_EQ(0, c.tx.channel);
    ASSERT_FALSE(c.tunnel.enabled);
    ASSERT_EQ(0, c.tunnel.peer_node);
    ASSERT_EQ(20, c.tunnel.idle_gap_ms);

    /* stored config is upgraded to the current schema */
    ASSERT_EQ(SQ_CONFIG_VERSION, c.schema_version);
}

void test_blob_decode_v3_tx_routing(void)
{
    uint8_t blob[BLOB_CAP];
    size_t n = golden_load("v3", blob, sizeof(blob));
    ASSERT_EQ(56, n);

    sq_config_t c;
    config_set_defaults(&c);
    ASSERT_TRUE(config_from_blob(&c, blob, n));

    ASSERT_STR_EQ("V3NODE", c.device_name);
    ASSERT_EQ(115200, c.modbus.baud);
    ASSERT_EQ(HAL_PARITY_EVEN, c.modbus.parity);
    ASSERT_EQ(250, c.modbus.response_timeout_ms);
    ASSERT_EQ(1, c.modbus.retries);
    ASSERT_EQ(5, c.modbus.poll_gap_ms);
    ASSERT_EQ(900, c.power.uplink_interval_s);

    ASSERT_EQ(2, c.poll_count);
    ASSERT_EQ(2, c.polls[0].slave);
    ASSERT_EQ(40, c.polls[0].reg_start);
    ASSERT_EQ(4, c.polls[0].reg_count);
    ASSERT_EQ(3, c.polls[1].slave);
    ASSERT_EQ(4, c.polls[1].function);

    ASSERT_EQ(0xDEADBEEF, c.tx.dest_node); /* v3 addition */
    ASSERT_EQ(3, c.tx.channel);
    ASSERT_TRUE(c.tx.confirmed); /* flags bit2 */

    ASSERT_FALSE(c.tunnel.enabled); /* still no tunnel block in v3 */
    ASSERT_EQ(20, c.tunnel.idle_gap_ms);
}

void test_blob_decode_v4_tunnel(void)
{
    uint8_t blob[BLOB_CAP];
    size_t n = golden_load("v4", blob, sizeof(blob));
    ASSERT_EQ(62, n);

    sq_config_t c;
    config_set_defaults(&c);
    ASSERT_TRUE(config_from_blob(&c, blob, n));

    ASSERT_STR_EQ("V4NODE", c.device_name);
    ASSERT_EQ(38400, c.modbus.baud);
    ASSERT_EQ(1500, c.modbus.response_timeout_ms);
    ASSERT_EQ(100, c.modbus.poll_gap_ms);

    ASSERT_EQ(2, c.poll_count);
    ASSERT_EQ(5, c.polls[0].slave);
    ASSERT_EQ(1000, c.polls[0].reg_start);
    ASSERT_EQ(16, c.polls[0].reg_count); /* SQ_MAX_REGS */

    ASSERT_EQ(0x01020304, c.tx.dest_node);
    ASSERT_EQ(7, c.tx.channel);
    ASSERT_TRUE(c.tx.confirmed);

    ASSERT_TRUE(c.tunnel.enabled); /* flags bit1 */
    ASSERT_EQ(0x11223344, c.tunnel.peer_node);
    ASSERT_EQ(50, c.tunnel.idle_gap_ms);
}

void test_blob_decode_at_maximum_size(void)
{
    uint8_t blob[BLOB_CAP];
    size_t n = golden_load("v4_max", blob, sizeof(blob));
    ASSERT_EQ(98, n); /* 37 hdr + 8*6 polls + 5 tx + 6 tunnel + 2 crc */

    sq_config_t c;
    config_set_defaults(&c);
    ASSERT_TRUE(config_from_blob(&c, blob, n));

    ASSERT_EQ(SQ_MAX_POLLS, c.poll_count);
    for (uint8_t i = 0; i < SQ_MAX_POLLS; i++) {
        ASSERT_EQ(i + 1, c.polls[i].slave);
        ASSERT_EQ(3, c.polls[i].function);
        ASSERT_EQ(i * 16, c.polls[i].reg_start);
        ASSERT_EQ(SQ_MAX_REGS, c.polls[i].reg_count);
    }
    ASSERT_EQ(0xFFFFFFFF, c.tx.dest_node);
    ASSERT_EQ(0xFFFFFFFF, c.tunnel.peer_node);
    ASSERT_EQ(65535, c.tunnel.idle_gap_ms);
}

/* ── decoding: what must be preserved ─────────────────────────────────────── */

/* Keys are provisioned separately from deployment config; a config download must
   never disturb them. */
void test_blob_decode_preserves_lorawan_credentials(void)
{
    uint8_t blob[BLOB_CAP];
    size_t n = golden_load("v4", blob, sizeof(blob));
    ASSERT_TRUE(n > 0);

    sq_config_t c;
    config_set_defaults(&c);
    plant_lorawan(&c);

    sq_lorawan_t before = c.lorawan;
    ASSERT_TRUE(config_from_blob(&c, blob, n));
    ASSERT_MEM_EQ(&before, &c.lorawan, sizeof(before));
}

void test_blob_roundtrip_is_idempotent(void)
{
    uint8_t first[BLOB_CAP];
    size_t n = golden_load("v4", first, sizeof(first));
    ASSERT_TRUE(n > 0);

    sq_config_t c;
    config_set_defaults(&c);
    ASSERT_TRUE(config_from_blob(&c, first, n));

    uint8_t second[BLOB_CAP];
    size_t m = config_to_blob(&c, second, sizeof(second));

    /* decode -> encode must reproduce the input exactly: anything else means a
       field is lost or defaulted on the way through. */
    ASSERT_EQ(n, m);
    ASSERT_MEM_EQ(first, second, n);
}

/* ── decoding: rejection paths ────────────────────────────────────────────── */

/* Every malformed blob must be rejected AND must leave the live config
   untouched — a half-applied config is worse than a refused one. */
static void assert_rejected_and_config_intact(const char *what, const uint8_t *blob, size_t len)
{
    sq_config_t c, before;
    config_set_defaults(&c);
    plant_lorawan(&c);
    before = c;

    if (config_from_blob(&c, blob, len))
        SQ__FAILED("expected rejection: %s", what);
    else
        sq_checks++;

    ASSERT_MEM_EQ(&before, &c, sizeof(before));
}

void test_blob_rejects_bad_magic(void)
{
    uint8_t blob[BLOB_CAP];
    size_t n = golden_load("v4", blob, sizeof(blob));
    ASSERT_TRUE(n > 0);

    blob[0] = 'X';
    assert_rejected_and_config_intact("magic byte 0", blob, n);

    blob[0] = 'S';
    blob[1] = 'X';
    assert_rejected_and_config_intact("magic byte 1", blob, n);
}

void test_blob_rejects_unknown_versions(void)
{
    uint8_t blob[BLOB_CAP];
    size_t n = golden_load("v4", blob, sizeof(blob));
    ASSERT_TRUE(n > 0);

    const uint8_t bad_versions[] = {0, 1, 5, 0xFF};
    for (size_t i = 0; i < sizeof(bad_versions); i++) {
        blob[2] = bad_versions[i];
        assert_rejected_and_config_intact("unsupported version", blob, n);
    }
}

void test_blob_rejects_crc_mismatch(void)
{
    uint8_t blob[BLOB_CAP];
    size_t n = golden_load("v4", blob, sizeof(blob));
    ASSERT_TRUE(n > 0);

    blob[10] ^= 0x01; /* one bit, in the middle of the name field */
    assert_rejected_and_config_intact("corrupted payload", blob, n);

    blob[10] ^= 0x01;    /* restore */
    blob[n - 1] ^= 0xFF; /* corrupt the CRC itself */
    assert_rejected_and_config_intact("corrupted crc", blob, n);
}

void test_blob_rejects_wrong_length(void)
{
    uint8_t blob[BLOB_CAP];
    size_t n = golden_load("v4", blob, sizeof(blob));
    ASSERT_TRUE(n > 0);

    assert_rejected_and_config_intact("one byte short", blob, n - 1);
    assert_rejected_and_config_intact("one byte long", blob, n + 1);
    assert_rejected_and_config_intact("header only", blob, 38); /* needs >= 37 + 2 */
    assert_rejected_and_config_intact("empty", blob, 0);
}

/* poll_count past SQ_MAX_POLLS on a blob that is otherwise perfectly formed —
   correct length, correct CRC. Nothing but the bounds check stands between this
   and a write past the end of polls[]. */
void test_blob_rejects_poll_count_overflow(void)
{
    uint8_t blob[BLOB_CAP];
    size_t n = golden_load("v4_overflow", blob, sizeof(blob));
    ASSERT_EQ(104, n); /* 37 + 9*6 + 5 + 6 + 2 */

    /* The fixture really is self-consistent: only poll_count is out of range. */
    ASSERT_EQ(9, blob[36]);
    uint16_t crc = (uint16_t)(blob[n - 2] | (blob[n - 1] << 8));
    ASSERT_EQ(crc, modbus_crc16(blob, n - 2));

    assert_rejected_and_config_intact("poll_count > SQ_MAX_POLLS", blob, n);
}
