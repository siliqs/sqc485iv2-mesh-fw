/* modbus.c — portable Modbus-RTU master, raw-forward output. */
#include "modbus.h"
#include "board_profile.h"
#include "hal/hal_serial.h"
#include "hal/hal_time.h"
#include <string.h>

uint16_t modbus_crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

bool modbus_exception_is_transient(uint8_t exception_code)
{
    switch (exception_code) {
    case SQ_MB_EXC_ILLEGAL_FUNCTION: /* this slave does not implement FC3/FC4  */
    case SQ_MB_EXC_ILLEGAL_ADDRESS:  /* the register is not there — wrong plan */
    case SQ_MB_EXC_ILLEGAL_VALUE:    /* the register count is out of range     */
    case SQ_MB_EXC_GATEWAY_PATH:     /* the gateway is misconfigured           */
        return false;
    default:
        /* 0x04 device failure, 0x05 acknowledge, 0x06 busy, 0x08 memory parity,
           0x0B gateway target silent — and anything unrecognised. All of these
           can clear on their own, so they keep the retry budget. */
        return true;
    }
}

/* Read exactly `want` bytes, or give up.

   hal_serial_read() spins until it has filled the buffer or the timeout expires —
   it does NOT return early when the line goes quiet. So every byte asked for and
   not delivered costs a full response_timeout_ms, which is why the reply is read
   in stages rather than as one blind request for the maximum length. */
static bool read_exact(uint8_t *dst, size_t want, uint32_t to_ms)
{
    size_t got = 0;
    while (got < want) {
        int n = hal_serial_read(dst + got, want - got, to_ms);
        if (n <= 0)
            return false;
        got += (size_t)n;
    }
    return true;
}

/* One transaction. Sends req, then reads the reply into body[] and reports how
   long it turned out to be — a Modbus exception is 5 bytes regardless of how many
   registers were asked for. Returns SQ_MB_OK or an error. */
static uint8_t transact(const sq_modbus_t *mb, const uint8_t *req, size_t reqlen, uint8_t *body, size_t exp, size_t *body_len)
{
    hal_serial_set_tx(true);
    hal_serial_write(req, reqlen);
    hal_serial_flush();
    hal_serial_set_tx(false);

    /* Half-duplex boards tie /RE low, so everything transmitted comes straight
       back. Drain exactly that much before listening for the slave. */
    if (BOARD.rs485_tx_echo) {
        uint8_t echo[8];
        if (reqlen > sizeof(echo))
            return SQ_MB_ERR_SHORT;
        if (!read_exact(echo, reqlen, mb->response_timeout_ms))
            return SQ_MB_ERR_TIMEOUT;
    }

    /* Address and function first: the function's high bit is what says whether
       the rest of this frame is a full reply or a 5-byte exception. Asking for
       `exp` bytes up front is what used to turn every exception into a timeout —
       the short frame never completed, so the reason for the failure was lost. */
    if (!read_exact(body, 2, mb->response_timeout_ms))
        return SQ_MB_ERR_TIMEOUT;

    size_t want = (body[1] & 0x80) ? SQ_MB_EXCEPTION_FRAME_LEN : exp;
    if (!read_exact(body + 2, want - 2, mb->response_timeout_ms))
        return SQ_MB_ERR_TIMEOUT;

    *body_len = want;
    return SQ_MB_OK;
}

size_t modbus_read_raw(const sq_modbus_t *mb, uint8_t slave, uint8_t func, uint16_t reg_start, uint16_t reg_count, uint8_t *out,
                       size_t cap, uint8_t *err)
{
    *err = SQ_MB_ERR_TIMEOUT;
    if (reg_count == 0 || reg_count > SQ_MAX_REGS) {
        *err = SQ_MB_ERR_SHORT;
        return 0;
    }

    uint8_t req[8];
    req[0] = slave;
    req[1] = func;
    req[2] = (uint8_t)(reg_start >> 8);
    req[3] = (uint8_t)(reg_start & 0xFF);
    req[4] = (uint8_t)(reg_count >> 8);
    req[5] = (uint8_t)(reg_count & 0xFF);
    uint16_t crc = modbus_crc16(req, 6);
    req[6] = (uint8_t)(crc & 0xFF);
    req[7] = (uint8_t)(crc >> 8);

    size_t exp = 5 + 2u * reg_count;    /* [addr][func][bc][data][crc_lo][crc_hi] */
    size_t outlen = 2 + 2u * reg_count; /* [addr][func][data] */
    if (outlen > cap) {
        *err = SQ_MB_ERR_SHORT;
        return 0;
    }

    int attempts = (int)mb->retries + 1;
    for (int a = 0; a < attempts; a++) {
        uint8_t body[5 + 2 * SQ_MAX_REGS];
        size_t blen = 0;
        uint8_t e = transact(mb, req, sizeof(req), body, exp, &blen);
        if (e == SQ_MB_OK) {
            /* Checksum first — no field is worth reading out of a frame that did
               not survive the wire. */
            uint16_t rx_crc = (uint16_t)(body[blen - 2] | (body[blen - 1] << 8));
            if (rx_crc != modbus_crc16(body, blen - 2)) {
                *err = SQ_MB_ERR_CRC;
            } else if (body[0] != slave) {
                *err = SQ_MB_ERR_MISMATCH;
            } else if (body[1] & 0x80) {
                /* The slave answered — it just refused. Whether asking again can
                   help is the slave's own answer to give, so honour it rather
                   than spending the whole retry budget on a request that is
                   wrong by construction. */
                *err = SQ_MB_ERR_EXCEPTION;
                if (!modbus_exception_is_transient(body[2]))
                    return 0;
            } else if (body[1] != func) {
                *err = SQ_MB_ERR_MISMATCH;
            } else if (body[2] != 2 * reg_count) {
                *err = SQ_MB_ERR_SHORT;
            } else {
                out[0] = body[0];                          /* addr */
                out[1] = body[1];                          /* func */
                memcpy(out + 2, body + 3, 2u * reg_count); /* data (skip bytecount) */
                *err = SQ_MB_OK;
                return outlen;
            }
        } else {
            *err = e;
        }
        hal_delay(20); /* inter-frame gap before retry */
    }
    return 0;
}
