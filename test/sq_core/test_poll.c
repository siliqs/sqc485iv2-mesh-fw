/*
 * test_poll.c — the raw-forward payload builder.
 *
 * The cloud decoder slices the concatenated payload using nothing but the poll
 * plan, so byte alignment is the whole contract: every poll must contribute
 * exactly 2 + 2*reg_count bytes whether it succeeded or not. A failed read that
 * shortened the payload would silently shift every later value in the cloud.
 */
#include <string.h>

#include "config.h"
#include "mock_hal.h"
#include "modbus.h"
#include "poll.h"
#include "sq_test.h"

#define PAY_CAP 256

static void fresh(void)
{
    mock_serial_reset();
    mock_time_reset();
    mock_board_reset();
}

/* A plan with retries disabled, so one scripted response maps to one poll. */
static void plan(sq_config_t *c, uint8_t count)
{
    config_set_defaults(c);
    c->modbus.retries = 0;
    c->poll_count = count;
}

void test_poll_concatenates_raw_responses(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);
    mock_serial_set_reg(0, 0x1111);
    mock_serial_set_reg(1, 0x2222);
    mock_serial_set_reg(10, 0x3333);

    sq_config_t c;
    plan(&c, 2);
    c.polls[0].slave = 1;
    c.polls[0].function = 3;
    c.polls[0].reg_start = 0;
    c.polls[0].reg_count = 2;
    c.polls[1].slave = 2;
    c.polls[1].function = 4;
    c.polls[1].reg_start = 10;
    c.polls[1].reg_count = 1;

    uint8_t pay[PAY_CAP];
    size_t n = poll_collect_raw(&c, pay, sizeof(pay));

    const uint8_t expect[10] = {
        0x01, 0x03, 0x11, 0x11, 0x22, 0x22, /* poll 0: [addr][func] + 2 regs */
        0x02, 0x04, 0x33, 0x33,             /* poll 1: [addr][func] + 1 reg  */
    };
    ASSERT_EQ(sizeof(expect), n);
    ASSERT_MEM_EQ(expect, pay, sizeof(expect));
}

/* The alignment guarantee: a dead slave produces a same-length error frame. */
void test_poll_failure_emits_same_length_error_frame(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_SILENT);

    sq_config_t c;
    plan(&c, 1);
    c.polls[0].slave = 9;
    c.polls[0].function = 3;
    c.polls[0].reg_start = 0;
    c.polls[0].reg_count = 3;

    uint8_t pay[PAY_CAP];
    size_t n = poll_collect_raw(&c, pay, sizeof(pay));

    ASSERT_EQ(8, n); /* 2 + 2*3 — identical to a successful read */
    ASSERT_EQ(0x09, pay[0]);
    ASSERT_EQ(0x83, pay[1]); /* function | 0x80, the Modbus exception convention */
    for (size_t i = 2; i < n; i++)
        ASSERT_EQ(SQ_MB_ERR_TIMEOUT, pay[i]); /* payload padded with the error code */
}

/* The case the alignment rule exists for: a failure in the MIDDLE of the plan
   must not shift the polls that follow it. */
void test_poll_mixed_results_stay_aligned(void)
{
    fresh();
    const mock_rsp_t script[] = {MOCK_RSP_GOOD, MOCK_RSP_SILENT, MOCK_RSP_GOOD};
    mock_serial_script(script, 3);
    mock_serial_set_reg(0, 0xAAAA);
    mock_serial_set_reg(20, 0xCCCC);

    sq_config_t c;
    plan(&c, 3);
    c.polls[0].slave = 1;
    c.polls[0].function = 3;
    c.polls[0].reg_start = 0;
    c.polls[0].reg_count = 1;
    c.polls[1].slave = 2;
    c.polls[1].function = 3;
    c.polls[1].reg_start = 10;
    c.polls[1].reg_count = 2;
    c.polls[2].slave = 3;
    c.polls[2].function = 3;
    c.polls[2].reg_start = 20;
    c.polls[2].reg_count = 1;

    uint8_t pay[PAY_CAP];
    size_t n = poll_collect_raw(&c, pay, sizeof(pay));

    const uint8_t expect[14] = {
        0x01, 0x03, 0xAA, 0xAA,             /* ok      */
        0x02, 0x83, 0x01, 0x01, 0x01, 0x01, /* failed, still 6 bytes */
        0x03, 0x03, 0xCC, 0xCC,             /* ok, at the right offset */
    };
    ASSERT_EQ(sizeof(expect), n);
    ASSERT_MEM_EQ(expect, pay, sizeof(expect));
}

/* Different failure kinds must be distinguishable in the payload. */
void test_poll_error_frame_carries_the_error_code(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_BAD_CRC);

    sq_config_t c;
    plan(&c, 1);
    c.polls[0].slave = 4;
    c.polls[0].function = 3;
    c.polls[0].reg_count = 1;

    uint8_t pay[PAY_CAP];
    size_t n = poll_collect_raw(&c, pay, sizeof(pay));

    ASSERT_EQ(4, n);
    ASSERT_EQ(0x04, pay[0]); /* slave address is preserved */
    ASSERT_EQ(0x83, pay[1]); /* function 3 | 0x80 */
    ASSERT_EQ(SQ_MB_ERR_CRC, pay[2]);
    ASSERT_EQ(SQ_MB_ERR_CRC, pay[3]);
}

/* Truncation must land on a poll boundary — half a poll would desync the cloud
   decoder for the whole rest of the payload. */
void test_poll_truncates_on_a_poll_boundary(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);

    sq_config_t c;
    plan(&c, 3);
    for (int i = 0; i < 3; i++) {
        c.polls[i].slave = (uint8_t)(i + 1);
        c.polls[i].function = 3;
        c.polls[i].reg_start = 0;
        c.polls[i].reg_count = 2; /* 6 bytes each */
    }

    uint8_t pay[PAY_CAP];

    ASSERT_EQ(18, poll_collect_raw(&c, pay, sizeof(pay))); /* all three fit */

    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);
    ASSERT_EQ(12, poll_collect_raw(&c, pay, 17)); /* room for 2.8 polls -> 2 polls */

    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);
    ASSERT_EQ(0, poll_collect_raw(&c, pay, 5)); /* not even one */
}

void test_poll_empty_plan_produces_nothing(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);

    sq_config_t c;
    plan(&c, 0);

    uint8_t pay[PAY_CAP];
    ASSERT_EQ(0, poll_collect_raw(&c, pay, sizeof(pay)));
    ASSERT_EQ(0, mock_serial_tx_count()); /* nothing driven onto the bus */
}

/* The settle delay goes BETWEEN polls, not after the last one. */
void test_poll_gap_is_applied_between_polls_only(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);

    sq_config_t c;
    plan(&c, 3);
    c.modbus.poll_gap_ms = 75;
    for (int i = 0; i < 3; i++) {
        c.polls[i].slave = (uint8_t)(i + 1);
        c.polls[i].function = 3;
        c.polls[i].reg_count = 1;
    }

    uint8_t pay[PAY_CAP];
    poll_collect_raw(&c, pay, sizeof(pay));

    ASSERT_EQ(2 * 75, mock_time_now()); /* 3 polls -> 2 gaps */
}

void test_poll_gap_zero_costs_nothing(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);

    sq_config_t c;
    plan(&c, 3);
    c.modbus.poll_gap_ms = 0;
    for (int i = 0; i < 3; i++) {
        c.polls[i].slave = (uint8_t)(i + 1);
        c.polls[i].function = 3;
        c.polls[i].reg_count = 1;
    }

    uint8_t pay[PAY_CAP];
    poll_collect_raw(&c, pay, sizeof(pay));

    ASSERT_EQ(0, mock_time_now());
}

/* The mesh module calls poll_collect_raw() with a buffer of exactly
   meshtastic_Constants_DATA_PAYLOAD_LEN bytes (ModbusModule.cpp:267). */
#define MESH_PAYLOAD_LEN 233

static void build_widest_plan(sq_config_t *c)
{
    plan(c, SQ_MAX_POLLS);
    c->modbus.poll_gap_ms = 0;
    for (int i = 0; i < SQ_MAX_POLLS; i++) {
        c->polls[i].slave = (uint8_t)(i + 1);
        c->polls[i].function = 3;
        c->polls[i].reg_start = (uint16_t)(i * SQ_MAX_REGS);
        c->polls[i].reg_count = SQ_MAX_REGS;
    }
}

void test_poll_widest_plan_produces_272_bytes(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);

    sq_config_t c;
    build_widest_plan(&c);

    uint8_t pay[320]; /* deliberately larger than any mesh payload */
    size_t n = poll_collect_raw(&c, pay, sizeof(pay));

    ASSERT_EQ(272, n); /* SQ_MAX_POLLS * (2 + 2*SQ_MAX_REGS) */
    ASSERT_EQ(SQ_MAX_POLLS, mock_serial_tx_count());
}

/* CAPACITY MISMATCH, pinned deliberately.
 *
 * The widest plan the config format can express (8 polls x 16 registers = 272
 * bytes) does not fit the 233-byte mesh payload the module hands it. Truncation
 * is graceful — it stops on a poll boundary, so the cloud decode of what DOES
 * arrive stays correct — but the last two polls are dropped from every single
 * uplink, silently, with nothing in the payload to say so.
 *
 * The configurator will happily accept such a plan. Either it should refuse
 * anything over 233 bytes, or poll_collect_raw()'s caller should log the drop.
 * Until then this test states the real limit: 6 full-width polls.
 */
void test_poll_drops_polls_that_exceed_the_mesh_payload(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);
    mock_serial_set_reg(0, 0xFEED);

    sq_config_t c;
    build_widest_plan(&c);

    uint8_t pay[MESH_PAYLOAD_LEN];
    size_t n = poll_collect_raw(&c, pay, sizeof(pay));

    ASSERT_EQ(6 * 34, n);                 /* 204 — poll 7 would need 238 */
    ASSERT_EQ(6, mock_serial_tx_count()); /* the dropped polls are not even read */
    ASSERT_EQ(0x01, pay[0]);              /* first poll present ... */
    ASSERT_EQ(0x06, pay[5 * 34]);         /* ... sixth is the last one */
}
