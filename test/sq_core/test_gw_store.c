/*
 * test_gw_store.c — the gateway's runtime telemetry cache.
 *
 * gw_store_put() has no serial I/O of its own — it only slices a byte buffer
 * the caller already has (a node's raw-forward uplink) using a map[] the
 * caller already has (the gateway's configured register map), and stamps a
 * clock the caller already has (hal_millis()). So unlike modbus_slave.c
 * (later, needs an injected-request mock) this module only needs the
 * existing mock_time virtual clock — no new mock capability required.
 */
#include <string.h>

#include "config.h"
#include "gw_store.h"
#include "mock_hal.h"
#include "sq_test.h"

static void fresh(void)
{
    gw_store_reset();
    mock_time_reset();
}

static void one_row_map(sq_gateway_reply_t *gw, uint32_t node_id, uint8_t slave, uint8_t func, uint16_t reg_start,
                         uint16_t reg_count)
{
    memset(gw, 0, sizeof(*gw));
    gw->map_count = 1;
    gw->map[0].node_id = node_id;
    gw->map[0].src_slave = slave;
    gw->map[0].src_func = func;
    gw->map[0].src_reg_start = reg_start;
    gw->map[0].src_reg_count = reg_count;
}

void test_gw_store_put_then_find_a_single_poll(void)
{
    fresh();
    sq_gateway_reply_t gw;
    one_row_map(&gw, 42, 3, 3, 100, 2);

    const uint8_t raw[6] = {0x03, 0x03, 0x11, 0x11, 0x22, 0x22};
    ASSERT_EQ(1, gw_store_put(&gw, 42, raw, sizeof(raw), 1000));

    uint8_t out[4];
    uint32_t last_seen = 0;
    ASSERT_TRUE(gw_store_find(3, 3, 100, 2, out, sizeof(out), &last_seen));
    ASSERT_MEM_EQ(raw + 2, out, sizeof(out));
    ASSERT_EQ(1000, last_seen);
}

void test_gw_store_find_misses_on_any_key_field(void)
{
    fresh();
    sq_gateway_reply_t gw;
    one_row_map(&gw, 42, 3, 3, 100, 2);
    const uint8_t raw[6] = {0x03, 0x03, 0x11, 0x11, 0x22, 0x22};
    gw_store_put(&gw, 42, raw, sizeof(raw), 1000);

    uint8_t out[4];
    ASSERT_FALSE(gw_store_find(9, 3, 100, 2, out, sizeof(out), NULL));   /* wrong slave     */
    ASSERT_FALSE(gw_store_find(3, 4, 100, 2, out, sizeof(out), NULL));   /* wrong function  */
    ASSERT_FALSE(gw_store_find(3, 3, 101, 2, out, sizeof(out), NULL));   /* wrong reg_start */
    ASSERT_FALSE(gw_store_find(3, 3, 100, 3, out, sizeof(out), NULL));   /* wrong reg_count */
}

/* A node's uplink is many polls concatenated with no length markers — the map
   (filtered to this node_id, in that node's own poll order) is the only thing
   that tells the store where each poll's bytes start and end. */
void test_gw_store_slices_multiple_polls_from_one_stream(void)
{
    fresh();
    sq_gateway_reply_t gw;
    memset(&gw, 0, sizeof(gw));
    gw.map_count = 2;
    gw.map[0] = (sq_gw_map_entry_t){.node_id = 7, .src_slave = 1, .src_func = 3, .src_reg_start = 0, .src_reg_count = 1};
    gw.map[1] = (sq_gw_map_entry_t){.node_id = 7, .src_slave = 2, .src_func = 4, .src_reg_start = 10, .src_reg_count = 2};

    const uint8_t raw[10] = {
        0x01, 0x03, 0xAA, 0xAA,             /* poll 0: 1 reg  */
        0x02, 0x04, 0xBB, 0xBB, 0xCC, 0xCC, /* poll 1: 2 regs */
    };
    ASSERT_EQ(2, gw_store_put(&gw, 7, raw, sizeof(raw), 500));

    uint8_t out[4];
    ASSERT_TRUE(gw_store_find(1, 3, 0, 1, out, sizeof(out), NULL));
    ASSERT_MEM_EQ(raw + 2, out, 2);
    ASSERT_TRUE(gw_store_find(2, 4, 10, 2, out, sizeof(out), NULL));
    ASSERT_MEM_EQ(raw + 6, out, 4);
}

/* Map rows for OTHER node_ids must not consume bytes from this node's
   stream — only this node's own rows advance the cursor. */
void test_gw_store_put_skips_other_nodes_rows_without_consuming_bytes(void)
{
    fresh();
    sq_gateway_reply_t gw;
    memset(&gw, 0, sizeof(gw));
    gw.map_count = 2;
    gw.map[0] = (sq_gw_map_entry_t){.node_id = 99, .src_slave = 5, .src_func = 3, .src_reg_start = 0, .src_reg_count = 5};
    gw.map[1] = (sq_gw_map_entry_t){.node_id = 7, .src_slave = 1, .src_func = 3, .src_reg_start = 0, .src_reg_count = 1};

    const uint8_t raw[4] = {0x01, 0x03, 0xAA, 0xAA}; /* only node 7's own poll */
    ASSERT_EQ(1, gw_store_put(&gw, 7, raw, sizeof(raw), 500));

    uint8_t out[2];
    ASSERT_TRUE(gw_store_find(1, 3, 0, 1, out, sizeof(out), NULL));
    ASSERT_FALSE(gw_store_find(5, 3, 0, 5, out, sizeof(out), NULL)); /* never populated */
}

/* An error frame ([slave][func|0x80][err...]) does not overwrite the cache —
   a transient hiccup on the far node must not clobber the last-good value. */
void test_gw_store_put_preserves_last_good_value_on_an_error_frame(void)
{
    fresh();
    sq_gateway_reply_t gw;
    one_row_map(&gw, 42, 3, 3, 100, 1);

    const uint8_t good[4] = {0x03, 0x03, 0x55, 0x66};
    ASSERT_EQ(1, gw_store_put(&gw, 42, good, sizeof(good), 1000));

    mock_time_advance(500);
    const uint8_t err_frame[4] = {0x03, 0x83, 0x01, 0x01}; /* func | 0x80 = error */
    ASSERT_EQ(0, gw_store_put(&gw, 42, err_frame, sizeof(err_frame), mock_time_now()));

    uint8_t out[2];
    uint32_t last_seen = 0;
    ASSERT_TRUE(gw_store_find(3, 3, 100, 1, out, sizeof(out), &last_seen));
    ASSERT_MEM_EQ(good + 2, out, 2);  /* still the good value */
    ASSERT_EQ(1000, last_seen);       /* last_seen_ms untouched */
}

/* A second good update to the same key updates in place, not a new slot. */
void test_gw_store_put_updates_an_existing_key_in_place(void)
{
    fresh();
    sq_gateway_reply_t gw;
    one_row_map(&gw, 42, 3, 3, 100, 1);

    const uint8_t first[4] = {0x03, 0x03, 0x00, 0x01};
    gw_store_put(&gw, 42, first, sizeof(first), 1000);

    const uint8_t second[4] = {0x03, 0x03, 0x00, 0x02};
    ASSERT_EQ(1, gw_store_put(&gw, 42, second, sizeof(second), 2000));

    uint8_t out[2];
    uint32_t last_seen = 0;
    ASSERT_TRUE(gw_store_find(3, 3, 100, 1, out, sizeof(out), &last_seen));
    ASSERT_MEM_EQ(second + 2, out, 2);
    ASSERT_EQ(2000, last_seen);
}

/* A stream too short to contain even the matching row's frame is a no-op —
   mirrors poll_collect_raw()'s own truncate-on-a-poll-boundary contract. */
void test_gw_store_put_stops_on_a_short_stream(void)
{
    fresh();
    sq_gateway_reply_t gw;
    one_row_map(&gw, 42, 3, 3, 100, 2); /* needs 6 bytes */

    const uint8_t raw[4] = {0x03, 0x03, 0x11, 0x11}; /* only 4 */
    ASSERT_EQ(0, gw_store_put(&gw, 42, raw, sizeof(raw), 1000));

    uint8_t out[4];
    ASSERT_FALSE(gw_store_find(3, 3, 100, 2, out, sizeof(out), NULL));
}

void test_gw_store_reset_clears_everything(void)
{
    fresh();
    sq_gateway_reply_t gw;
    one_row_map(&gw, 42, 3, 3, 100, 1);
    const uint8_t raw[4] = {0x03, 0x03, 0x11, 0x11};
    gw_store_put(&gw, 42, raw, sizeof(raw), 1000);

    gw_store_reset();

    uint8_t out[2];
    ASSERT_FALSE(gw_store_find(3, 3, 100, 1, out, sizeof(out), NULL));
}
