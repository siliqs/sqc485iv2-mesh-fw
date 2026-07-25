/*
 * mock_serial.c — scriptable RS485 line.
 *
 * Acts as a Modbus-RTU slave that can be told, per attempt, exactly how to
 * misbehave. That is what makes modbus.c's error branches reachable from a host
 * test: a real slave will not politely produce a CRC error on request.
 *
 * The TX echo the real hardware produces (/RE tied low) is reproduced when
 * BOARD.rs485_tx_echo is set, so the engine's echo-stripping is under test too.
 */
#include <string.h>

#include "board_profile.h"
#include "hal/hal_serial.h"
#include "mock_hal.h"
#include "modbus.h"

#define MAX_ATTEMPTS 16
#define MAX_FRAME 264
#define REG_FILE 64

static uint8_t s_rx[MAX_FRAME * 2];
static size_t s_rx_len, s_rx_pos;

static mock_rsp_t s_script[MAX_ATTEMPTS];
static size_t s_script_len;
static size_t s_attempt;

static uint16_t s_regs[REG_FILE];
static bool s_reg_set[REG_FILE];
static uint8_t s_exc_code = 0x02;
static bool s_corrupt_crc;

static uint8_t s_tx[MAX_ATTEMPTS][MAX_FRAME];
static size_t s_tx_len[MAX_ATTEMPTS];
static size_t s_tx_count;

static char s_log[512];
static size_t s_log_len;

static void logc(char c)
{
    if (s_log_len + 1 < sizeof(s_log))
        s_log[s_log_len++] = c;
    s_log[s_log_len] = '\0';
}

static uint16_t sim_reg(uint16_t addr)
{
    if (addr < REG_FILE && s_reg_set[addr])
        return s_regs[addr];
    return (uint16_t)(1000 + addr);
}

static void append_crc(uint8_t *f, size_t *p)
{
    uint16_t crc = modbus_crc16(f, *p);
    f[(*p)++] = (uint8_t)(crc & 0xFF);
    f[(*p)++] = (uint8_t)(crc >> 8);
}

/* Build the slave's answer to one request, honouring the scripted fault. */
static size_t build_response(mock_rsp_t mode, const uint8_t *req, uint8_t *out)
{
    uint8_t slave = req[0], func = req[1];
    uint16_t rs = (uint16_t)((req[2] << 8) | req[3]);
    uint16_t cnt = (uint16_t)((req[4] << 8) | req[5]);
    size_t p = 0;

    if (mode == MOCK_RSP_SILENT)
        return 0;

    if (mode == MOCK_RSP_EXCEPTION) {
        out[p++] = slave;
        out[p++] = (uint8_t)(func | 0x80);
        out[p++] = s_exc_code;
        append_crc(out, &p);
        if (s_corrupt_crc)
            out[p - 1] ^= 0xFF;
        return p;
    }

    out[p++] = (mode == MOCK_RSP_WRONG_SLAVE) ? (uint8_t)(slave + 1) : slave;
    out[p++] = (mode == MOCK_RSP_WRONG_FUNC) ? (uint8_t)(func + 1) : func;
    out[p++] = (mode == MOCK_RSP_BAD_BYTECOUNT) ? (uint8_t)(cnt * 2 + 1) : (uint8_t)(cnt * 2);
    for (uint16_t i = 0; i < cnt; i++) {
        uint16_t v = sim_reg((uint16_t)(rs + i));
        out[p++] = (uint8_t)(v >> 8);
        out[p++] = (uint8_t)(v & 0xFF);
    }
    append_crc(out, &p);

    if (mode == MOCK_RSP_BAD_CRC || s_corrupt_crc)
        out[p - 1] ^= 0xFF; /* corrupt the CRC only — everything else stays valid */

    return p;
}

/* ── HAL implementation ───────────────────────────────────────────────────── */

bool hal_serial_init(uint32_t baud, hal_parity_t parity, uint8_t stop_bits)
{
    (void)baud;
    (void)parity;
    (void)stop_bits;
    return true;
}

void hal_serial_set_tx(bool enable)
{
    logc(enable ? '+' : '-');
}

void hal_serial_flush(void)
{
    logc('F');
}

int hal_serial_write(const uint8_t *buf, size_t len)
{
    logc('W');

    if (s_tx_count < MAX_ATTEMPTS) {
        size_t n = len < MAX_FRAME ? len : MAX_FRAME;
        memcpy(s_tx[s_tx_count], buf, n);
        s_tx_len[s_tx_count] = n;
    }
    s_tx_count++;

    if (len < 8)
        return (int)len; /* not a complete RTU request — stay silent */

    mock_rsp_t mode = MOCK_RSP_GOOD;
    if (s_script_len)
        mode = s_script[s_attempt < s_script_len ? s_attempt : s_script_len - 1];
    s_attempt++;

    s_rx_len = s_rx_pos = 0;
    if (BOARD.rs485_tx_echo) { /* the line echoes what we transmitted */
        memcpy(s_rx, buf, len);
        s_rx_len = len;
    }

    uint8_t rsp[MAX_FRAME];
    size_t n = build_response(mode, buf, rsp);
    memcpy(s_rx + s_rx_len, rsp, n);
    s_rx_len += n;

    return (int)len;
}

int hal_serial_read(uint8_t *buf, size_t len, uint32_t to_ms)
{
    logc('R');

    size_t avail = s_rx_len - s_rx_pos;
    size_t n = len < avail ? len : avail;
    if (n) {
        memcpy(buf, s_rx + s_rx_pos, n);
        s_rx_pos += n;
    }

    /* Fidelity matters here. The real HAL (hal_meshtastic.cpp) spins until it has
       filled `len` bytes OR the timeout expires — it does NOT return early just
       because the line went quiet. So asking for more bytes than will ever arrive
       always costs a full response_timeout_ms, even when some bytes did turn up.
       A mock that returned partial data immediately made the engine look twice as
       fast as it is on a dead bus. */
    if (n < len)
        mock_time_advance(to_ms);

    return (int)n;
}

/* ── test control ─────────────────────────────────────────────────────────── */

void mock_serial_reset(void)
{
    s_rx_len = s_rx_pos = 0;
    s_script_len = 0;
    s_attempt = 0;
    s_tx_count = 0;
    s_log_len = 0;
    s_log[0] = '\0';
    s_exc_code = 0x02;
    s_corrupt_crc = false;
    memset(s_reg_set, 0, sizeof(s_reg_set));
    memset(s_tx_len, 0, sizeof(s_tx_len));
}

void mock_serial_set_mode(mock_rsp_t mode)
{
    s_script[0] = mode;
    s_script_len = 1;
    s_attempt = 0;
}

void mock_serial_script(const mock_rsp_t *modes, size_t n)
{
    if (n > MAX_ATTEMPTS)
        n = MAX_ATTEMPTS;
    memcpy(s_script, modes, n * sizeof(*modes));
    s_script_len = n;
    s_attempt = 0;
}

void mock_serial_set_reg(uint16_t addr, uint16_t value)
{
    if (addr < REG_FILE) {
        s_regs[addr] = value;
        s_reg_set[addr] = true;
    }
}

void mock_serial_set_exception_code(uint8_t code)
{
    s_exc_code = code;
}

void mock_serial_corrupt_crc(bool on)
{
    s_corrupt_crc = on;
}

size_t mock_serial_tx_count(void)
{
    return s_tx_count;
}

const uint8_t *mock_serial_tx(size_t i, size_t *len)
{
    if (i >= s_tx_count || i >= MAX_ATTEMPTS)
        return NULL;
    if (len)
        *len = s_tx_len[i];
    return s_tx[i];
}

const char *mock_serial_log(void)
{
    return s_log;
}
