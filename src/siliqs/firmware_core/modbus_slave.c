/* modbus_slave.c — see modbus_slave.h for the responder's role. */
#include "modbus_slave.h"
#include "board_profile.h"
#include "gw_store.h"
#include "hal/hal_serial.h"
#include "hal/hal_time.h"
#include "modbus.h"
#include <string.h>

/* A valid FC3/FC4 read request is always exactly this long:
   [slave][func][reg_start_hi][reg_start_lo][reg_count_hi][reg_count_lo][crc_lo][crc_hi]. */
#define REQ_LEN 8

/* Slack past REQ_LEN so an oversized/garbage frame is still fully absorbed by
   receive_frame() in one pass instead of bleeding its tail into the next
   frame's timing — anything longer than REQ_LEN is dropped by the length
   check below regardless. */
#define MAX_FRAME 16

/* [slave][func][bytecount][data...][crc_lo][crc_hi] */
#define MAX_REPLY (3u + 2u * SQ_MAX_REGS + 2u)

/* T3.5 (MODBUS Application Protocol V1.1b3 §2.5.1.1): 3.5 character times of
   silence marks a frame boundary; one character is ~11 bit-times (start + 8
   data + parity/stop framing). Above 19200 baud the spec pins this to a flat
   1750us instead of scaling further down. hal_time.h / hal_serial_read()
   only have millisecond granularity — there is no microsecond primitive in
   the HAL — so the computed value is rounded UP to whole milliseconds. That
   is the safe direction: a slightly longer required gap never mistakes
   normal inter-byte jitter for end-of-frame, at the cost of at most ~1ms of
   extra reply latency. */
static uint32_t t35_ms(uint32_t baud)
{
    uint32_t us = (baud > 19200) ? 1750u : (uint32_t)((38500000u + baud - 1u) / baud);
    uint32_t ms = (us + 999u) / 1000u;
    return ms ? ms : 1u;
}

/* hal_serial_read() spins until it has filled the buffer or the timeout
   expires — it does NOT return early when the line goes quiet (see modbus.c's
   read_exact()). Asking for exactly 1 byte is what turns that into "return
   the instant a byte arrives, else wait out the full gap and report
   nothing", which is the primitive T3.5 framing actually needs. */
static bool read_gap_byte(uint8_t *out, uint32_t gap_ms)
{
    return hal_serial_read(out, 1, gap_ms) == 1;
}

static size_t receive_frame(uint8_t *buf, size_t cap, uint32_t gap_ms)
{
    size_t n = 0;
    while (n < cap && read_gap_byte(&buf[n], gap_ms))
        n++;
    return n;
}

/* Absorb whatever is left of an oversized/garbage frame so its tail doesn't
   get mistaken for the start of the next one. */
static void drain_until_gap(uint32_t gap_ms)
{
    uint8_t junk;
    while (read_gap_byte(&junk, gap_ms)) { }
}

/* True if `slave` appears as some row's src_slave — i.e. it is an address
   this node's transparent mode would answer under, independent of which
   func/regs that row covers (this only establishes ownership of the
   address; the request's own func/reg_start/reg_count are used as-is once
   matched — transparent mode never translates). */
static bool slave_in_map(const sq_gateway_reply_t *gw, uint8_t slave)
{
    for (uint8_t i = 0; i < gw->map_count; i++)
        if (gw->map[i].src_slave == slave)
            return true;
    return false;
}

/* Aggregated mode: the wire request's (func, reg_start, reg_count) must land
   on exactly one virtual register range — no partial/overlapping virtual
   reads, mirroring gw_store_find()'s own exact-match rule. Returns the
   matching row, or NULL. */
static const sq_gw_map_entry_t *resolve_aggregated(const sq_gateway_reply_t *gw, uint8_t func, uint16_t reg_start,
                                                     uint16_t reg_count)
{
    for (uint8_t i = 0; i < gw->map_count; i++) {
        const sq_gw_map_entry_t *row = &gw->map[i];
        if (row->src_func == func && row->virt_reg_offset == reg_start && row->src_reg_count == reg_count)
            return row;
    }
    return NULL;
}

static void send_frame(const uint8_t *frame, size_t len, uint32_t gap_ms)
{
    hal_serial_set_tx(true);
    hal_serial_write(frame, len);
    hal_serial_flush();
    hal_serial_set_tx(false);

    /* Half-duplex boards tie /RE low, so our own reply echoes straight back
       on RX — drain it before the next poll's receive_frame() runs, the same
       precaution modbus.c's transact() takes on the master side. */
    if (BOARD.rs485_tx_echo) {
        uint8_t junk;
        for (size_t i = 0; i < len; i++)
            if (!read_gap_byte(&junk, gap_ms))
                break;
    }
}

static void send_exception(uint8_t slave, uint8_t func, uint8_t code, uint32_t gap_ms)
{
    uint8_t f[SQ_MB_EXCEPTION_FRAME_LEN];
    f[0] = slave;
    f[1] = (uint8_t)(func | 0x80);
    f[2] = code;
    uint16_t crc = modbus_crc16(f, 3);
    f[3] = (uint8_t)(crc & 0xFF);
    f[4] = (uint8_t)(crc >> 8);
    send_frame(f, sizeof(f), gap_ms);
}

static void send_reply(uint8_t slave, uint8_t func, const uint8_t *data, uint16_t reg_count, uint32_t gap_ms)
{
    uint8_t f[MAX_REPLY];
    size_t p = 0;
    f[p++] = slave;
    f[p++] = func;
    f[p++] = (uint8_t)(2u * reg_count);
    memcpy(f + p, data, 2u * reg_count);
    p += 2u * reg_count;
    uint16_t crc = modbus_crc16(f, p);
    f[p++] = (uint8_t)(crc & 0xFF);
    f[p++] = (uint8_t)(crc >> 8);
    send_frame(f, p, gap_ms);
}

bool modbus_slave_poll(const sq_config_t *cfg)
{
    const sq_gateway_reply_t *gw = &cfg->gateway;
    if (!gw->transparent_enabled && !gw->aggregated_enabled)
        return false;

    uint32_t gap_ms = t35_ms(cfg->modbus.baud);

    uint8_t frame[MAX_FRAME];
    size_t n = receive_frame(frame, sizeof(frame), gap_ms);
    if (n == 0)
        return false; /* bus idle for a full frame-gap */

    if (n == sizeof(frame)) {
        drain_until_gap(gap_ms); /* oversized/garbage — flush the rest, don't answer */
        return true;
    }
    if (n != REQ_LEN)
        return true; /* not a valid FC3/FC4 request length — silently drop */

    uint16_t rx_crc = (uint16_t)(frame[6] | (frame[7] << 8));
    if (rx_crc != modbus_crc16(frame, 6))
        return true; /* corrupt frame — a slave that can't verify it must not answer */

    uint8_t slave = frame[0];
    uint8_t func = frame[1];
    uint16_t reg_start = (uint16_t)((frame[2] << 8) | frame[3]);
    uint16_t reg_count = (uint16_t)((frame[4] << 8) | frame[5]);

    /* Aggregated takes priority when both modes are enabled and the address
       matches both — it is the more specific identity, one address standing
       in for many nodes' worth of virtual registers. */
    bool as_aggregated = gw->aggregated_enabled && slave == gw->aggregated_slave_addr;
    bool as_transparent = !as_aggregated && gw->transparent_enabled && slave_in_map(gw, slave);
    if (!as_aggregated && !as_transparent)
        return true; /* not one of our identities — stay silent, like a real slave would for a foreign address */

    if (func != 3 && func != 4) {
        send_exception(slave, func, SQ_MB_EXC_ILLEGAL_FUNCTION, gap_ms);
        return true;
    }

    uint8_t src_slave, src_func;
    uint16_t src_reg_start, src_reg_count;
    if (as_aggregated) {
        const sq_gw_map_entry_t *row = resolve_aggregated(gw, func, reg_start, reg_count);
        if (!row) {
            send_exception(slave, func, SQ_MB_EXC_ILLEGAL_ADDRESS, gap_ms);
            return true;
        }
        src_slave = row->src_slave;
        src_func = row->src_func;
        src_reg_start = row->src_reg_start;
        src_reg_count = row->src_reg_count;
    } else {
        src_slave = slave;
        src_func = func;
        src_reg_start = reg_start;
        src_reg_count = reg_count;
    }

    uint8_t data[GW_STORE_MAX_DATA_LEN];
    uint32_t last_seen_ms;
    if (!gw_store_find(src_slave, src_func, src_reg_start, src_reg_count, data, sizeof(data), &last_seen_ms)) {
        send_exception(slave, func, SQ_MB_EXC_ILLEGAL_ADDRESS, gap_ms);
        return true;
    }

    /* 0 means "no staleness limit configured" — config_set_defaults() and
       every legacy-blob back-fill path always set this to 3600s, so 0 is only
       reachable by deliberate configuration — rather than "instantly stale",
       which would make the whole feature a silent footgun. */
    if (gw->staleness_threshold_s != 0) {
        uint32_t age_s = (hal_millis() - last_seen_ms) / 1000u; /* unsigned subtraction wraps safely */
        if (age_s > gw->staleness_threshold_s) {
            send_exception(slave, func, SQ_MB_EXC_GATEWAY_NO_RESPONSE, gap_ms);
            return true;
        }
    }

    send_reply(slave, func, data, src_reg_count, gap_ms);
    return true;
}
