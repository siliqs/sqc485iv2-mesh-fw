/* modbus.c — portable Modbus-RTU master, raw-forward output. */
#include <string.h>
#include "modbus.h"
#include "board_profile.h"
#include "hal/hal_serial.h"
#include "hal/hal_time.h"

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

/* One transaction. Sends req, reads exp response bytes (after stripping the TX
   echo per BOARD), copies them to body[0..exp-1]. Returns SQ_MB_OK or an error. */
static uint8_t transact(const sq_modbus_t *mb, const uint8_t *req, size_t reqlen,
                        uint8_t *body, size_t exp)
{
    hal_serial_set_tx(true);
    hal_serial_write(req, reqlen);
    hal_serial_flush();
    hal_serial_set_tx(false);

    size_t echo  = BOARD.rs485_tx_echo ? reqlen : 0;
    size_t total = echo + exp;

    uint8_t buf[8 + 5 + 2 * SQ_MAX_REGS + 8];
    if (total > sizeof(buf)) return SQ_MB_ERR_SHORT;

    size_t got = 0;
    while (got < total) {
        int n = hal_serial_read(buf + got, total - got, mb->response_timeout_ms);
        if (n <= 0) break;
        got += (size_t)n;
    }
    if (got < total) return SQ_MB_ERR_TIMEOUT;

    memcpy(body, buf + echo, exp);
    return SQ_MB_OK;
}

size_t modbus_read_raw(const sq_modbus_t *mb, uint8_t slave, uint8_t func,
                       uint16_t reg_start, uint16_t reg_count,
                       uint8_t *out, size_t cap, uint8_t *err)
{
    *err = SQ_MB_ERR_TIMEOUT;
    if (reg_count == 0 || reg_count > SQ_MAX_REGS) { *err = SQ_MB_ERR_SHORT; return 0; }

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

    size_t exp    = 5 + 2u * reg_count;   /* [addr][func][bc][data][crc_lo][crc_hi] */
    size_t outlen = 2 + 2u * reg_count;   /* [addr][func][data] */
    if (outlen > cap) { *err = SQ_MB_ERR_SHORT; return 0; }

    int attempts = (int)mb->retries + 1;
    for (int a = 0; a < attempts; a++) {
        uint8_t body[5 + 2 * SQ_MAX_REGS];
        uint8_t e = transact(mb, req, sizeof(req), body, exp);
        if (e == SQ_MB_OK) {
            if (body[1] & 0x80)                  { *err = SQ_MB_ERR_EXCEPTION; }
            else if (body[0] != slave || body[1] != func) { *err = SQ_MB_ERR_MISMATCH; }
            else if ((uint16_t)(body[exp - 2] | (body[exp - 1] << 8)) != modbus_crc16(body, exp - 2))
                                                 { *err = SQ_MB_ERR_CRC; }
            else if (body[2] != 2 * reg_count)   { *err = SQ_MB_ERR_SHORT; }
            else {
                out[0] = body[0];                /* addr */
                out[1] = body[1];                /* func */
                memcpy(out + 2, body + 3, 2u * reg_count);   /* data (skip bytecount) */
                *err = SQ_MB_OK;
                return outlen;
            }
        } else {
            *err = e;
        }
        hal_delay(20);   /* inter-frame gap before retry */
    }
    return 0;
}
