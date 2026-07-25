/*
 * test_modbus.c — the RTU master transaction.
 *
 * Covers the parts that only misbehave against a real, badly-behaved bus:
 * TX-echo stripping, DE turnaround order, and every rejection branch. The
 * scriptable mock makes each of those reachable deterministically.
 */
#include <string.h>

#include "config.h"
#include "mock_hal.h"
#include "modbus.h"
#include "sq_test.h"

#define OUT_CAP 64

static sq_modbus_t link_defaults(void)
{
    sq_config_t c;
    config_set_defaults(&c); /* 9600 8N1, 1000 ms timeout, 3 retries, 20 ms gap */
    return c.modbus;
}

static void fresh(void)
{
    mock_serial_reset();
    mock_time_reset();
    mock_board_reset();
}

/* ── the happy path ───────────────────────────────────────────────────────── */

void test_modbus_reads_holding_registers(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);
    mock_serial_set_reg(0, 0x1234);
    mock_serial_set_reg(1, 0x5678);

    sq_modbus_t mb = link_defaults();
    uint8_t out[OUT_CAP], err = 0xFF;
    size_t n = modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, NULL);

    ASSERT_EQ(SQ_MB_OK, err);
    ASSERT_EQ(6, n); /* raw-forward = [addr][func] + 2*reg_count, no bytecount, no CRC */

    const uint8_t expect[6] = {0x01, 0x03, 0x12, 0x34, 0x56, 0x78};
    ASSERT_MEM_EQ(expect, out, sizeof(expect));

    ASSERT_EQ(1, mock_serial_tx_count()); /* no retry needed */
}

/* The exact bytes that hit the wire, including CRC byte order (low first). */
void test_modbus_request_frame_is_well_formed(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);

    sq_modbus_t mb = link_defaults();
    uint8_t out[OUT_CAP], err;
    modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, NULL);

    size_t len = 0;
    const uint8_t *tx = mock_serial_tx(0, &len);
    ASSERT_TRUE(tx != NULL);
    ASSERT_EQ(8, len);

    const uint8_t expect[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B};
    ASSERT_MEM_EQ(expect, tx, sizeof(expect));
}

void test_modbus_encodes_16bit_register_fields(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);

    sq_modbus_t mb = link_defaults();
    uint8_t out[OUT_CAP], err;
    modbus_read_raw(&mb, 0x11, 4, 0x0ABC, 16, out, sizeof(out), &err, NULL);
    ASSERT_EQ(SQ_MB_OK, err);

    size_t len = 0;
    const uint8_t *tx = mock_serial_tx(0, &len);
    ASSERT_TRUE(tx != NULL);
    ASSERT_EQ(0x11, tx[0]);
    ASSERT_EQ(0x04, tx[1]);
    ASSERT_EQ(0x0A, tx[2]); /* reg_start big-endian, per Modbus */
    ASSERT_EQ(0xBC, tx[3]);
    ASSERT_EQ(0x00, tx[4]); /* reg_count big-endian */
    ASSERT_EQ(0x10, tx[5]);
}

/* DE must be asserted before the first bit and released before listening, or the
   transceiver keeps driving the bus and the slave's reply is lost. */
void test_modbus_de_turnaround_order(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);

    sq_modbus_t mb = link_defaults();
    uint8_t out[OUT_CAP], err;
    modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, NULL);

    /* '+' tx on, 'W' write, 'F' flush, '-' tx off, 'R' read */
    const char *log = mock_serial_log();
    ASSERT_EQ(0, strncmp(log, "+WF-R", 5));
}

void test_modbus_max_register_count(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);

    sq_modbus_t mb = link_defaults();
    uint8_t out[OUT_CAP], err;
    size_t n = modbus_read_raw(&mb, 1, 3, 0, SQ_MAX_REGS, out, sizeof(out), &err, NULL);

    ASSERT_EQ(SQ_MB_OK, err);
    ASSERT_EQ(2 + 2 * SQ_MAX_REGS, n);
}

/* ── TX echo ──────────────────────────────────────────────────────────────── */

/* On the SQC485I the transceiver's /RE is tied low, so everything transmitted is
   read straight back. The engine must drop exactly reqlen bytes — and must NOT
   drop them on a board that does not echo. */
void test_modbus_handles_both_echo_polarities(void)
{
    const uint8_t expect[6] = {0x01, 0x03, 0x12, 0x34, 0x56, 0x78};

    for (int echo = 0; echo <= 1; echo++) {
        fresh();
        mock_board()->rs485_tx_echo = (bool)echo;
        mock_serial_set_mode(MOCK_RSP_GOOD);
        mock_serial_set_reg(0, 0x1234);
        mock_serial_set_reg(1, 0x5678);

        sq_modbus_t mb = link_defaults();
        uint8_t out[OUT_CAP], err = 0xFF;
        size_t n = modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, NULL);

        ASSERT_EQ(SQ_MB_OK, err);
        ASSERT_EQ(6, n);
        ASSERT_MEM_EQ(expect, out, sizeof(expect));
    }
}

/* ── rejection paths ──────────────────────────────────────────────────────── */

static void assert_read_fails(mock_rsp_t mode, uint8_t expect_err, size_t expect_attempts)
{
    fresh();
    mock_serial_set_mode(mode);

    sq_modbus_t mb = link_defaults();
    uint8_t out[OUT_CAP], err = 0xFF;
    size_t n = modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, NULL);

    ASSERT_EQ(0, n);
    ASSERT_EQ(expect_err, err);
    ASSERT_EQ(expect_attempts, mock_serial_tx_count());
}

void test_modbus_silent_bus_times_out(void)
{
    assert_read_fails(MOCK_RSP_SILENT, SQ_MB_ERR_TIMEOUT, 4); /* retries=3 -> 4 attempts */
}

void test_modbus_rejects_corrupt_crc(void)
{
    assert_read_fails(MOCK_RSP_BAD_CRC, SQ_MB_ERR_CRC, 4);
}

void test_modbus_rejects_wrong_byte_count(void)
{
    assert_read_fails(MOCK_RSP_BAD_BYTECOUNT, SQ_MB_ERR_SHORT, 4);
}

void test_modbus_rejects_foreign_slave_reply(void)
{
    assert_read_fails(MOCK_RSP_WRONG_SLAVE, SQ_MB_ERR_MISMATCH, 4);
}

void test_modbus_rejects_wrong_function_reply(void)
{
    assert_read_fails(MOCK_RSP_WRONG_FUNC, SQ_MB_ERR_MISMATCH, 4);
}

/* A slave that answers "illegal data address" — the usual symptom of a wrong
 * register in the poll plan — must be distinguishable from an unplugged cable.
 * An exception frame is 5 bytes whatever was asked for, so the engine has to read
 * the function byte before it can know how much more to expect. */
void test_modbus_reports_exception_replies(void)
{
    const uint8_t codes[] = {0x01, 0x02, 0x03, 0x04}; /* illegal fn/address/value, device failure */

    for (size_t i = 0; i < sizeof(codes); i++) {
        fresh();
        mock_serial_set_mode(MOCK_RSP_EXCEPTION);
        mock_serial_set_exception_code(codes[i]);

        sq_modbus_t mb = link_defaults();
        uint8_t out[OUT_CAP], err = 0xFF;
        size_t n = modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, NULL);

        ASSERT_EQ(0, n);
        ASSERT_EQ(SQ_MB_ERR_EXCEPTION, err);
    }
}

/* Knowing the slave refused is not enough to act on. 0x02 "register absent" means
 * the poll plan is wrong; 0x06 "device busy" means the plan is fine and the device
 * is not. So the code comes back to the caller. */
void test_modbus_reports_the_exception_code(void)
{
    const uint8_t codes[] = {SQ_MB_EXC_ILLEGAL_FUNCTION, SQ_MB_EXC_ILLEGAL_ADDRESS, SQ_MB_EXC_DEVICE_BUSY, 0x7F};

    for (size_t i = 0; i < sizeof(codes); i++) {
        fresh();
        mock_serial_set_mode(MOCK_RSP_EXCEPTION);
        mock_serial_set_exception_code(codes[i]);

        sq_modbus_t mb = link_defaults();
        uint8_t out[OUT_CAP], err = 0xFF, exc = 0xFF;
        modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, &exc);

        ASSERT_EQ(SQ_MB_ERR_EXCEPTION, err);
        ASSERT_EQ(codes[i], exc);
    }
}

/* Anything that is not a refusal must leave the code at 0, so a caller cannot
 * mistake a stale value for a reason. */
void test_modbus_exception_code_is_zero_for_other_outcomes(void)
{
    const struct {
        mock_rsp_t mode;
        uint8_t expect_err;
    } cases[] = {
        {MOCK_RSP_GOOD, SQ_MB_OK},
        {MOCK_RSP_SILENT, SQ_MB_ERR_TIMEOUT},
        {MOCK_RSP_BAD_CRC, SQ_MB_ERR_CRC},
        {MOCK_RSP_WRONG_SLAVE, SQ_MB_ERR_MISMATCH},
        {MOCK_RSP_BAD_BYTECOUNT, SQ_MB_ERR_SHORT},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fresh();
        mock_serial_set_mode(cases[i].mode);

        sq_modbus_t mb = link_defaults();
        uint8_t out[OUT_CAP], err = 0xFF, exc = 0xAA;
        modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, &exc);

        ASSERT_EQ(cases[i].expect_err, err);
        ASSERT_EQ(0, exc);
    }

    /* Rejected before anything reaches the bus. */
    fresh();
    sq_modbus_t mb = link_defaults();
    uint8_t out[OUT_CAP], err, exc = 0xAA;
    modbus_read_raw(&mb, 1, 3, 0, 0, out, sizeof(out), &err, &exc);
    ASSERT_EQ(0, exc);
}

/* Which refusals are worth repeating is the slave's own answer to give — the
 * Modbus spec splits its exception codes into "your request is wrong" and "ask
 * me again later", and the engine follows that split. */
void test_modbus_exception_transience_classification(void)
{
    /* Wrong by construction: repeating the identical request cannot help. */
    ASSERT_FALSE(modbus_exception_is_transient(SQ_MB_EXC_ILLEGAL_FUNCTION));
    ASSERT_FALSE(modbus_exception_is_transient(SQ_MB_EXC_ILLEGAL_ADDRESS));
    ASSERT_FALSE(modbus_exception_is_transient(SQ_MB_EXC_ILLEGAL_VALUE));
    ASSERT_FALSE(modbus_exception_is_transient(SQ_MB_EXC_GATEWAY_PATH));

    /* The slave understood and wants to be asked again. */
    ASSERT_TRUE(modbus_exception_is_transient(SQ_MB_EXC_DEVICE_FAILURE));
    ASSERT_TRUE(modbus_exception_is_transient(SQ_MB_EXC_ACKNOWLEDGE));
    ASSERT_TRUE(modbus_exception_is_transient(SQ_MB_EXC_DEVICE_BUSY));
    ASSERT_TRUE(modbus_exception_is_transient(SQ_MB_EXC_MEMORY_PARITY));
    ASSERT_TRUE(modbus_exception_is_transient(SQ_MB_EXC_GATEWAY_NO_RESPONSE));

    /* An unfamiliar code still gets the benefit of the doubt: a slave we do not
       recognise should not be written off on its first refusal. */
    ASSERT_TRUE(modbus_exception_is_transient(0x7F));
    ASSERT_TRUE(modbus_exception_is_transient(0x00));
}

/* "Illegal data address" is the usual symptom of a wrong register in the poll
 * plan. Asking three more times cannot change the answer — it just spends bus
 * time, and on a battery node, wake time. */
void test_modbus_permanent_exception_stops_on_the_first_answer(void)
{
    const uint8_t permanent[] = {SQ_MB_EXC_ILLEGAL_FUNCTION, SQ_MB_EXC_ILLEGAL_ADDRESS, SQ_MB_EXC_ILLEGAL_VALUE,
                                 SQ_MB_EXC_GATEWAY_PATH};

    for (size_t i = 0; i < sizeof(permanent); i++) {
        fresh();
        mock_serial_set_mode(MOCK_RSP_EXCEPTION);
        mock_serial_set_exception_code(permanent[i]);

        sq_modbus_t mb = link_defaults(); /* retries = 3 */
        uint8_t out[OUT_CAP], err = 0xFF;
        size_t n = modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, NULL);

        ASSERT_EQ(0, n);
        ASSERT_EQ(SQ_MB_ERR_EXCEPTION, err);
        ASSERT_EQ(1, mock_serial_tx_count()); /* answered once, believed once */
        ASSERT_EQ(0, mock_time_now());        /* not even the inter-frame gap */
    }
}

/* "Device busy" is the opposite: the slave understood and asked to be tried
 * again, so it gets the full budget. */
void test_modbus_transient_exception_uses_the_retry_budget(void)
{
    const uint8_t transient[] = {SQ_MB_EXC_DEVICE_FAILURE, SQ_MB_EXC_ACKNOWLEDGE, SQ_MB_EXC_DEVICE_BUSY,
                                 SQ_MB_EXC_GATEWAY_NO_RESPONSE, 0x7F};

    for (size_t i = 0; i < sizeof(transient); i++) {
        fresh();
        mock_serial_set_mode(MOCK_RSP_EXCEPTION);
        mock_serial_set_exception_code(transient[i]);

        sq_modbus_t mb = link_defaults();
        uint8_t out[OUT_CAP], err = 0xFF;
        modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, NULL);

        ASSERT_EQ(SQ_MB_ERR_EXCEPTION, err);
        ASSERT_EQ(4, mock_serial_tx_count()); /* retries=3 -> 4 attempts */
    }
}

/* A slave that is busy and then ready must be read on the retry, not written
 * off — the whole point of keeping the budget for transient codes. */
void test_modbus_recovers_after_a_busy_exception(void)
{
    fresh();
    const mock_rsp_t script[] = {MOCK_RSP_EXCEPTION, MOCK_RSP_GOOD};
    mock_serial_script(script, 2);
    mock_serial_set_exception_code(SQ_MB_EXC_DEVICE_BUSY);
    mock_serial_set_reg(0, 0x0BEE);

    sq_modbus_t mb = link_defaults();
    uint8_t out[OUT_CAP], err = 0xFF;
    size_t n = modbus_read_raw(&mb, 1, 3, 0, 1, out, sizeof(out), &err, NULL);

    ASSERT_EQ(SQ_MB_OK, err);
    ASSERT_EQ(4, n);
    ASSERT_EQ(0x0B, out[2]);
    ASSERT_EQ(0xEE, out[3]);
    ASSERT_EQ(2, mock_serial_tx_count());
}

/* A corrupted exception frame is a CRC failure, not an exception: nothing in a
 * frame that failed its checksum is worth reporting as fact. */
void test_modbus_corrupt_exception_frame_is_a_crc_error(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_EXCEPTION);
    mock_serial_corrupt_crc(true);

    sq_modbus_t mb = link_defaults();
    uint8_t out[OUT_CAP], err = 0xFF;
    modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, NULL);

    ASSERT_EQ(SQ_MB_ERR_CRC, err);
}

/* Bad arguments must be caught before anything is driven onto the bus. */
void test_modbus_rejects_invalid_register_count(void)
{
    sq_modbus_t mb = link_defaults();
    uint8_t out[OUT_CAP], err;

    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);
    ASSERT_EQ(0, modbus_read_raw(&mb, 1, 3, 0, 0, out, sizeof(out), &err, NULL));
    ASSERT_EQ(SQ_MB_ERR_SHORT, err);
    ASSERT_EQ(0, mock_serial_tx_count());

    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);
    ASSERT_EQ(0, modbus_read_raw(&mb, 1, 3, 0, SQ_MAX_REGS + 1, out, sizeof(out), &err, NULL));
    ASSERT_EQ(SQ_MB_ERR_SHORT, err);
    ASSERT_EQ(0, mock_serial_tx_count());
}

void test_modbus_rejects_undersized_output_buffer(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);

    sq_modbus_t mb = link_defaults();
    uint8_t out[OUT_CAP], err;
    ASSERT_EQ(0, modbus_read_raw(&mb, 1, 3, 0, 2, out, 5, &err, NULL)); /* needs 6 */
    ASSERT_EQ(SQ_MB_ERR_SHORT, err);
    ASSERT_EQ(0, mock_serial_tx_count());
}

/* ── retries ──────────────────────────────────────────────────────────────── */

void test_modbus_retry_recovers_from_a_glitch(void)
{
    fresh();
    const mock_rsp_t script[] = {MOCK_RSP_BAD_CRC, MOCK_RSP_GOOD};
    mock_serial_script(script, 2);
    mock_serial_set_reg(0, 0xAABB);

    sq_modbus_t mb = link_defaults();
    uint8_t out[OUT_CAP], err = 0xFF;
    size_t n = modbus_read_raw(&mb, 1, 3, 0, 1, out, sizeof(out), &err, NULL);

    ASSERT_EQ(SQ_MB_OK, err);
    ASSERT_EQ(4, n);
    ASSERT_EQ(0xAA, out[2]);
    ASSERT_EQ(0xBB, out[3]);
    ASSERT_EQ(2, mock_serial_tx_count()); /* stopped as soon as it succeeded */
}

void test_modbus_retry_count_is_honoured(void)
{
    const uint8_t retries[] = {0, 1, 5};

    for (size_t i = 0; i < sizeof(retries); i++) {
        fresh();
        mock_serial_set_mode(MOCK_RSP_SILENT);

        sq_modbus_t mb = link_defaults();
        mb.retries = retries[i];

        uint8_t out[OUT_CAP], err;
        ASSERT_EQ(0, modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, NULL));
        ASSERT_EQ(retries[i] + 1, mock_serial_tx_count());
    }
}

/* A dead bus must not stall the poll loop for longer than the configured budget:
   every attempt costs one response timeout plus the 20 ms inter-frame gap. */
void test_modbus_timeout_budget_is_bounded(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_SILENT);

    sq_modbus_t mb = link_defaults();
    mb.response_timeout_ms = 250;
    mb.retries = 2;

    uint8_t out[OUT_CAP], err;
    modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, NULL);

    ASSERT_EQ(3 * (250 + 20), mock_time_now());
}

/* A successful read returns immediately — no inter-frame delay is charged. */
void test_modbus_success_costs_no_delay(void)
{
    fresh();
    mock_serial_set_mode(MOCK_RSP_GOOD);

    sq_modbus_t mb = link_defaults();
    uint8_t out[OUT_CAP], err;
    modbus_read_raw(&mb, 1, 3, 0, 2, out, sizeof(out), &err, NULL);

    ASSERT_EQ(SQ_MB_OK, err);
    ASSERT_EQ(0, mock_time_now());
}
