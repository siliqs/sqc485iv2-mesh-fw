/*
 * test_modbus_slave.c — the gateway's passive T3.5 Modbus RTU slave responder.
 *
 * Unlike modbus.c (an RTU MASTER, driven by the code under test writing a
 * request and a scripted mock slave answering it), modbus_slave.c is the
 * SLAVE: it only ever reads first. mock_serial_inject() is the mirror-image
 * mock capability this needs — it puts an external master's request straight
 * on the wire for modbus_slave_poll() to read, and switches the mock's write
 * handler into "echo our own reply" mode instead of "synthesize a master's
 * answer" mode (see mock_serial.c's s_slave_mode).
 */
#include <string.h>

#include "config.h"
#include "gw_store.h"
#include "mock_hal.h"
#include "modbus.h"
#include "modbus_slave.h"
#include "sq_test.h"

static void fresh(void)
{
    mock_serial_reset();
    mock_time_reset();
    mock_board_reset();
    gw_store_reset();
}

static sq_config_t base_cfg(void)
{
    sq_config_t c;
    config_set_defaults(&c); /* 9600 8N1; gateway reply off, staleness 3600s */
    return c;
}

static void build_request(uint8_t slave, uint8_t func, uint16_t reg_start, uint16_t reg_count, uint8_t out[8])
{
    out[0] = slave;
    out[1] = func;
    out[2] = (uint8_t)(reg_start >> 8);
    out[3] = (uint8_t)(reg_start & 0xFF);
    out[4] = (uint8_t)(reg_count >> 8);
    out[5] = (uint8_t)(reg_count & 0xFF);
    uint16_t crc = modbus_crc16(out, 6);
    out[6] = (uint8_t)(crc & 0xFF);
    out[7] = (uint8_t)(crc >> 8);
}

static void assert_reply(uint8_t exp_slave, uint8_t exp_func, const uint8_t *exp_data, size_t exp_data_len)
{
    ASSERT_EQ(1, mock_serial_tx_count());
    size_t len;
    const uint8_t *f = mock_serial_tx(0, &len);
    ASSERT_EQ(3 + exp_data_len + 2, len);
    if (len < 3 + exp_data_len + 2)
        return;
    ASSERT_EQ(exp_slave, f[0]);
    ASSERT_EQ(exp_func, f[1]);
    ASSERT_EQ(exp_data_len, f[2]);
    ASSERT_MEM_EQ(exp_data, f + 3, exp_data_len);
    uint16_t crc = modbus_crc16(f, len - 2);
    ASSERT_EQ(crc & 0xFF, f[len - 2]);
    ASSERT_EQ(crc >> 8, f[len - 1]);
}

static void assert_exception(uint8_t exp_slave, uint8_t exp_func, uint8_t exp_code)
{
    ASSERT_EQ(1, mock_serial_tx_count());
    size_t len;
    const uint8_t *f = mock_serial_tx(0, &len);
    ASSERT_EQ(SQ_MB_EXCEPTION_FRAME_LEN, len);
    if (len < SQ_MB_EXCEPTION_FRAME_LEN)
        return;
    ASSERT_EQ(exp_slave, f[0]);
    ASSERT_EQ((exp_func | 0x80), f[1]);
    ASSERT_EQ(exp_code, f[2]);
}

static void assert_no_reply(void)
{
    ASSERT_EQ(0, mock_serial_tx_count());
}

/* ── idle bus / disabled ──────────────────────────────────────────────────── */

void test_modbus_slave_returns_false_when_bus_is_idle(void)
{
    fresh();
    sq_config_t c = base_cfg();
    c.gateway.transparent_enabled = true;
    c.gateway.map_count = 1;
    c.gateway.map[0] = (sq_gw_map_entry_t){.node_id = 7, .src_slave = 3, .src_func = 3, .src_reg_start = 100, .src_reg_count = 2};

    ASSERT_FALSE(modbus_slave_poll(&c));
    assert_no_reply();
}

void test_modbus_slave_noop_when_gateway_reply_disabled(void)
{
    fresh();
    sq_config_t c = base_cfg(); /* transparent/aggregated both default false */

    uint8_t req[8];
    build_request(3, 3, 100, 2, req);
    mock_serial_inject(req, sizeof(req));

    ASSERT_FALSE(modbus_slave_poll(&c));
    ASSERT_STR_EQ("", mock_serial_log()); /* never even touched the bus */
    assert_no_reply();
}

/* ── happy paths ──────────────────────────────────────────────────────────── */

void test_modbus_slave_transparent_reply_from_cache(void)
{
    fresh();
    sq_config_t c = base_cfg();
    c.gateway.transparent_enabled = true;
    c.gateway.map_count = 1;
    c.gateway.map[0] = (sq_gw_map_entry_t){.node_id = 7, .src_slave = 3, .src_func = 3, .src_reg_start = 100, .src_reg_count = 2};

    const uint8_t raw[6] = {0x03, 0x03, 0x11, 0x11, 0x22, 0x22};
    /* stamp at the current virtual time — hal_millis() only advances forward from
       here (the T3.5 gap timeout during receive_frame() costs it a few ms), so a
       later "now - last_seen" staleness check can never underflow */
    ASSERT_EQ(1, gw_store_put(&c.gateway, 7, raw, sizeof(raw), mock_time_now()));

    uint8_t req[8];
    build_request(3, 3, 100, 2, req);
    mock_serial_inject(req, sizeof(req));

    ASSERT_TRUE(modbus_slave_poll(&c));
    assert_reply(3, 3, raw + 2, 4);
}

void test_modbus_slave_aggregated_reply_resolves_via_map(void)
{
    fresh();
    sq_config_t c = base_cfg();
    c.gateway.aggregated_enabled = true;
    c.gateway.aggregated_slave_addr = 9;
    c.gateway.map_count = 1;
    c.gateway.map[0] = (sq_gw_map_entry_t){
        .node_id = 7, .src_slave = 3, .src_func = 3, .src_reg_start = 100, .src_reg_count = 2, .virt_reg_offset = 50};

    const uint8_t raw[6] = {0x03, 0x03, 0xAA, 0xAA, 0xBB, 0xBB};
    /* stamp at the current virtual time — see the identical comment in the
       transparent-mode test above for why this avoids a staleness underflow */
    ASSERT_EQ(1, gw_store_put(&c.gateway, 7, raw, sizeof(raw), mock_time_now()));

    /* the wire request addresses the aggregated slave + virtual offset, not
       the remote node's own address */
    uint8_t req[8];
    build_request(9, 3, 50, 2, req);
    mock_serial_inject(req, sizeof(req));

    ASSERT_TRUE(modbus_slave_poll(&c));
    assert_reply(9, 3, raw + 2, 4); /* replies under the request's own slave id */
}

/* ── address / dispatch edge cases ───────────────────────────────────────── */

void test_modbus_slave_ignores_a_frame_addressed_elsewhere(void)
{
    fresh();
    sq_config_t c = base_cfg();
    c.gateway.transparent_enabled = true;
    c.gateway.map_count = 1;
    c.gateway.map[0] = (sq_gw_map_entry_t){.node_id = 7, .src_slave = 3, .src_func = 3, .src_reg_start = 100, .src_reg_count = 2};

    uint8_t req[8];
    build_request(5 /* neither our transparent slave nor an aggregated addr */, 3, 100, 2, req);
    mock_serial_inject(req, sizeof(req));

    ASSERT_TRUE(modbus_slave_poll(&c)); /* a frame was seen on the wire */
    assert_no_reply();                  /* but not one of ours -> stay silent */
}

void test_modbus_slave_wrong_function_replies_illegal_function(void)
{
    fresh();
    sq_config_t c = base_cfg();
    c.gateway.transparent_enabled = true;
    c.gateway.map_count = 1;
    c.gateway.map[0] = (sq_gw_map_entry_t){.node_id = 7, .src_slave = 3, .src_func = 3, .src_reg_start = 100, .src_reg_count = 2};

    uint8_t req[8];
    build_request(3, 6 /* write single register — unsupported by this responder */, 100, 2, req);
    mock_serial_inject(req, sizeof(req));

    ASSERT_TRUE(modbus_slave_poll(&c));
    assert_exception(3, 6, SQ_MB_EXC_ILLEGAL_FUNCTION);
}

void test_modbus_slave_cache_miss_replies_illegal_address(void)
{
    fresh();
    sq_config_t c = base_cfg();
    c.gateway.transparent_enabled = true;
    c.gateway.map_count = 1;
    c.gateway.map[0] = (sq_gw_map_entry_t){.node_id = 7, .src_slave = 3, .src_func = 3, .src_reg_start = 100, .src_reg_count = 2};
    /* no gw_store_put — the cache never got a value for this key */

    uint8_t req[8];
    build_request(3, 3, 100, 2, req);
    mock_serial_inject(req, sizeof(req));

    ASSERT_TRUE(modbus_slave_poll(&c));
    assert_exception(3, 3, SQ_MB_EXC_ILLEGAL_ADDRESS);
}

void test_modbus_slave_aggregated_unregistered_reg_replies_illegal_address(void)
{
    fresh();
    sq_config_t c = base_cfg();
    c.gateway.aggregated_enabled = true;
    c.gateway.aggregated_slave_addr = 9;
    c.gateway.map_count = 1;
    c.gateway.map[0] = (sq_gw_map_entry_t){
        .node_id = 7, .src_slave = 3, .src_func = 3, .src_reg_start = 100, .src_reg_count = 2, .virt_reg_offset = 50};

    uint8_t req[8];
    build_request(9, 3, 999 /* not a virt_reg_offset any row registered */, 2, req);
    mock_serial_inject(req, sizeof(req));

    ASSERT_TRUE(modbus_slave_poll(&c));
    assert_exception(9, 3, SQ_MB_EXC_ILLEGAL_ADDRESS);
}

/* ── staleness ────────────────────────────────────────────────────────────── */

void test_modbus_slave_stale_entry_replies_gateway_no_response(void)
{
    fresh();
    sq_config_t c = base_cfg();
    c.gateway.transparent_enabled = true;
    c.gateway.staleness_threshold_s = 10;
    c.gateway.map_count = 1;
    c.gateway.map[0] = (sq_gw_map_entry_t){.node_id = 7, .src_slave = 3, .src_func = 3, .src_reg_start = 100, .src_reg_count = 2};

    const uint8_t raw[6] = {0x03, 0x03, 0x11, 0x11, 0x22, 0x22};
    gw_store_put(&c.gateway, 7, raw, sizeof(raw), 0);
    mock_time_advance(11000); /* 11s > 10s threshold */

    uint8_t req[8];
    build_request(3, 3, 100, 2, req);
    mock_serial_inject(req, sizeof(req));

    ASSERT_TRUE(modbus_slave_poll(&c));
    assert_exception(3, 3, SQ_MB_EXC_GATEWAY_NO_RESPONSE);
}

void test_modbus_slave_staleness_threshold_zero_disables_the_check(void)
{
    fresh();
    sq_config_t c = base_cfg();
    c.gateway.transparent_enabled = true;
    c.gateway.staleness_threshold_s = 0; /* explicitly disabled, not "expire instantly" */
    c.gateway.map_count = 1;
    c.gateway.map[0] = (sq_gw_map_entry_t){.node_id = 7, .src_slave = 3, .src_func = 3, .src_reg_start = 100, .src_reg_count = 2};

    const uint8_t raw[6] = {0x03, 0x03, 0x11, 0x11, 0x22, 0x22};
    gw_store_put(&c.gateway, 7, raw, sizeof(raw), 0);
    mock_time_advance(100000000); /* absurdly old */

    uint8_t req[8];
    build_request(3, 3, 100, 2, req);
    mock_serial_inject(req, sizeof(req));

    ASSERT_TRUE(modbus_slave_poll(&c));
    assert_reply(3, 3, raw + 2, 4);
}

/* ── malformed / noisy wire ──────────────────────────────────────────────── */

void test_modbus_slave_bad_crc_frame_is_silently_ignored(void)
{
    fresh();
    sq_config_t c = base_cfg();
    c.gateway.transparent_enabled = true;
    c.gateway.map_count = 1;
    c.gateway.map[0] = (sq_gw_map_entry_t){.node_id = 7, .src_slave = 3, .src_func = 3, .src_reg_start = 100, .src_reg_count = 2};

    uint8_t req[8];
    build_request(3, 3, 100, 2, req);
    req[7] ^= 0xFF; /* corrupt the CRC */
    mock_serial_inject(req, sizeof(req));

    ASSERT_TRUE(modbus_slave_poll(&c)); /* a frame was seen ... */
    assert_no_reply();                  /* ... but the spec forbids answering a bad CRC */
}

void test_modbus_slave_wrong_length_frame_is_silently_ignored(void)
{
    fresh();
    sq_config_t c = base_cfg();
    c.gateway.transparent_enabled = true;
    c.gateway.map_count = 1;
    c.gateway.map[0] = (sq_gw_map_entry_t){.node_id = 7, .src_slave = 3, .src_func = 3, .src_reg_start = 100, .src_reg_count = 2};

    const uint8_t garbage[5] = {0x03, 0x03, 0x00, 0x64, 0x00}; /* not 8 bytes */
    mock_serial_inject(garbage, sizeof(garbage));

    ASSERT_TRUE(modbus_slave_poll(&c));
    assert_no_reply();
}

void test_modbus_slave_oversized_frame_is_drained_without_reply(void)
{
    fresh();
    sq_config_t c = base_cfg();
    c.gateway.transparent_enabled = true;
    c.gateway.map_count = 1;
    c.gateway.map[0] = (sq_gw_map_entry_t){.node_id = 7, .src_slave = 3, .src_func = 3, .src_reg_start = 100, .src_reg_count = 2};

    uint8_t garbage[20]; /* wider than modbus_slave.c's MAX_FRAME(16) receive cap */
    memset(garbage, 0xEE, sizeof(garbage));
    mock_serial_inject(garbage, sizeof(garbage));

    ASSERT_TRUE(modbus_slave_poll(&c)); /* something was on the wire ... */
    assert_no_reply();                  /* ... but it can't be a valid request at that length */
}
