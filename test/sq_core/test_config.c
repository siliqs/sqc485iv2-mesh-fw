/*
 * test_config.c — defaults, the persisted-struct ABI, and load/save.
 *
 * The ABI test is the most important one in the suite. config_load() only
 * accepts a stored record whose length is exactly sizeof(sq_config_t); anything
 * else silently falls back to defaults. So adding a field to sq_config_t wipes
 * the configuration of every device already in the field on its next boot. That
 * is a decision, not an accident — this test forces it to be made deliberately.
 */
#include <string.h>

#include "config.h"
#include "mock_hal.h"
#include "sq_test.h"

#define CONFIG_KEY "cfg"

static void fresh(void)
{
    mock_store_reset();
    mock_time_reset();
    mock_board_reset();
}

/* ── the frozen contract ──────────────────────────────────────────────────── */

void test_config_struct_abi_is_frozen(void)
{
    /* Changing this number means fielded devices factory-reset on upgrade.
       If that is intended, bump SQ_CONFIG_VERSION, ship a migration, and update
       the number here in the same commit. */
    ASSERT_EQ(500, sizeof(sq_config_t));

    /* Sub-struct sizes, so a mismatch above points straight at the culprit. */
    ASSERT_EQ(48, sizeof(sq_poll_t));
    ASSERT_EQ(36, sizeof(sq_lorawan_t));
    ASSERT_EQ(16, sizeof(sq_modbus_t));
    ASSERT_EQ(8, sizeof(sq_tx_t));
    ASSERT_EQ(12, sizeof(sq_tunnel_t));

    /* 4-byte alignment and no 64-bit members is what keeps the host layout
       identical to the ESP32-C3 one — which is what makes this test meaningful. */
    ASSERT_EQ(4, _Alignof(sq_config_t));
}

void test_config_limits_are_frozen(void)
{
    ASSERT_EQ(4, SQ_CONFIG_VERSION);
    ASSERT_EQ(8, SQ_MAX_POLLS);
    ASSERT_EQ(16, SQ_MAX_REGS);
    ASSERT_EQ(24, SQ_NAME_LEN);

    /* Capability handshake: the configurator gates its UI on these. */
    ASSERT_EQ(2, SQ_CAP_PROTO);
    ASSERT_EQ(0x3F, SQ_FEATURES); /* tunnel|ble_power|rs485_term|poll_now|deep_sleep|get_config */
}

/* ── defaults ─────────────────────────────────────────────────────────────── */

void test_config_defaults(void)
{
    sq_config_t c;
    config_set_defaults(&c);

    ASSERT_EQ(SQ_CONFIG_VERSION, c.schema_version);
    ASSERT_STR_EQ("SQC485I", c.device_name);

    ASSERT_EQ(9600, c.modbus.baud);
    ASSERT_EQ(HAL_PARITY_NONE, c.modbus.parity);
    ASSERT_EQ(1, c.modbus.stop_bits);
    ASSERT_EQ(1000, c.modbus.response_timeout_ms);
    ASSERT_EQ(3, c.modbus.retries);
    ASSERT_EQ(20, c.modbus.poll_gap_ms);

    ASSERT_EQ(60, c.power.uplink_interval_s);
    ASSERT_TRUE(c.power.deep_sleep);

    ASSERT_TRUE(c.rs485_enabled);
    ASSERT_EQ(0, c.tx.dest_node); /* broadcast */
    ASSERT_EQ(0, c.tx.channel);   /* primary  */
    ASSERT_FALSE(c.tx.confirmed);

    ASSERT_FALSE(c.tunnel.enabled);
    ASSERT_EQ(0, c.tunnel.peer_node);
    ASSERT_EQ(20, c.tunnel.idle_gap_ms);

    /* The shipped example poll plan. poll[2] targets an absent slave on purpose:
       it is what demonstrates the fixed-length error frame. */
    ASSERT_EQ(3, c.poll_count);
    ASSERT_EQ(1, c.polls[0].slave);
    ASSERT_EQ(3, c.polls[0].function);
    ASSERT_EQ(0, c.polls[0].reg_start);
    ASSERT_EQ(2, c.polls[0].reg_count);
    ASSERT_EQ(SQ_TYPE_RAW, c.polls[0].type);
    ASSERT_EQ(8, c.polls[1].reg_start);
    ASSERT_EQ(SQ_TYPE_I16, c.polls[1].type);
    ASSERT_FLOAT_EQ(0.1f, c.polls[1].scale);
    ASSERT_EQ(9, c.polls[2].slave);

    /* Unused poll slots must be zeroed — the blob encoder trusts poll_count, but
       anything reading past it (logging, the configurator export) should see
       clean slots rather than stack rubbish. */
    for (int i = c.poll_count; i < SQ_MAX_POLLS; i++) {
        ASSERT_EQ(0, c.polls[i].slave);
        ASSERT_EQ(0, c.polls[i].reg_count);
    }
}

/* ── persistence ──────────────────────────────────────────────────────────── */

void test_config_save_load_roundtrip(void)
{
    fresh();

    sq_config_t saved;
    config_set_defaults(&saved);
    strncpy(saved.device_name, "field-unit-7", SQ_NAME_LEN - 1);
    saved.modbus.baud = 38400;
    saved.tx.dest_node = 0xCAFEBABE;
    saved.tunnel.enabled = true;
    ASSERT_TRUE(config_save(&saved));

    sq_config_t loaded;
    memset(&loaded, 0xAA, sizeof(loaded));
    ASSERT_TRUE(config_load(&loaded)); /* true = a stored config was used */
    ASSERT_MEM_EQ(&saved, &loaded, sizeof(saved));
}

void test_config_load_from_blank_store_yields_defaults(void)
{
    fresh();

    sq_config_t c;
    memset(&c, 0xAA, sizeof(c));
    ASSERT_FALSE(config_load(&c)); /* false = nothing stored, defaults applied */

    sq_config_t expect;
    config_set_defaults(&expect);
    ASSERT_MEM_EQ(&expect, &c, sizeof(expect));
}

/* A record written by a firmware whose sq_config_t was a different size. */
void test_config_load_rejects_wrong_sized_record(void)
{
    fresh();

    uint8_t old_record[sizeof(sq_config_t) - 4];
    memset(old_record, 0x5A, sizeof(old_record));
    mock_store_put_raw(CONFIG_KEY, old_record, sizeof(old_record));

    sq_config_t c;
    ASSERT_FALSE(config_load(&c));
    ASSERT_EQ(SQ_CONFIG_VERSION, c.schema_version);
    ASSERT_STR_EQ("SQC485I", c.device_name); /* fell back to defaults, not garbage */
}

void test_config_load_rejects_wrong_schema_version(void)
{
    fresh();

    sq_config_t stale;
    config_set_defaults(&stale);
    stale.schema_version = SQ_CONFIG_VERSION - 1;
    strncpy(stale.device_name, "stale", SQ_NAME_LEN - 1);
    mock_store_put_raw(CONFIG_KEY, &stale, sizeof(stale));

    sq_config_t c;
    ASSERT_FALSE(config_load(&c));
    ASSERT_STR_EQ("SQC485I", c.device_name);
}

void test_config_factory_reset_persists(void)
{
    fresh();

    sq_config_t c;
    config_set_defaults(&c);
    strncpy(c.device_name, "before-reset", SQ_NAME_LEN - 1);
    c.modbus.baud = 115200;
    ASSERT_TRUE(config_save(&c));

    ASSERT_TRUE(config_factory_reset(&c));
    ASSERT_STR_EQ("SQC485I", c.device_name);
    ASSERT_EQ(9600, c.modbus.baud);

    /* The reset must have hit the store, not just the struct. */
    sq_config_t reloaded;
    ASSERT_TRUE(config_load(&reloaded));
    ASSERT_STR_EQ("SQC485I", reloaded.device_name);
    ASSERT_EQ(9600, reloaded.modbus.baud);
}

/* ── type name helpers (used by provisioning + logs) ──────────────────────── */

void test_config_type_names_roundtrip(void)
{
    const struct {
        sq_datatype_t t;
        const char *name;
        const char *parse;
    } cases[] = {
        {SQ_TYPE_RAW, "RAW", "raw"}, {SQ_TYPE_U16, "U16", "u16"}, {SQ_TYPE_I16, "I16", "i16"},
        {SQ_TYPE_U32, "U32", "u32"}, {SQ_TYPE_I32, "I32", "i32"}, {SQ_TYPE_F32, "F32", "f32"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ASSERT_STR_EQ(cases[i].name, sq_type_name(cases[i].t));

        sq_datatype_t parsed = (sq_datatype_t)0xFF;
        ASSERT_TRUE(sq_type_parse(cases[i].parse, &parsed));
        ASSERT_EQ(cases[i].t, parsed);
    }

    sq_datatype_t unchanged = SQ_TYPE_U32;
    ASSERT_FALSE(sq_type_parse("nonsense", &unchanged));
    ASSERT_EQ(SQ_TYPE_U32, unchanged); /* a failed parse must not clobber the out param */

    ASSERT_STR_EQ("?", sq_type_name((sq_datatype_t)99));
}
