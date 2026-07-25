/* modbus.h — portable Modbus-RTU master engine (raw-forward).
   Reusable verbatim in both firmware versions (Star app core + Meshtastic module). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"

enum {
    SQ_MB_OK = 0,
    SQ_MB_ERR_TIMEOUT   = 1,   /* nothing (or not enough) came back in time     */
    SQ_MB_ERR_CRC       = 2,   /* frame arrived corrupted                       */
    SQ_MB_ERR_EXCEPTION = 3,   /* the slave answered, refusing the request      */
    SQ_MB_ERR_SHORT     = 4,   /* bad byte count, or a request we cannot make   */
    SQ_MB_ERR_MISMATCH  = 5    /* someone else's reply, or the wrong function   */
};

/* [addr][func|0x80][code][crc][crc] — an exception is this length whatever was
   asked for, which is why the reply length cannot be assumed up front. */
#define SQ_MB_EXCEPTION_FRAME_LEN 5

/* Standard Modbus RTU CRC16. */
uint16_t modbus_crc16(const uint8_t *buf, size_t len);

/* Do one read transaction and return the RAW forward bytes: [addr][func][data...]
   (bytecount and CRC dropped — the raw-forward wire format). Handles DE,
   TX-echo strip, CRC validation and retries. Returns the byte count written to
   out (= 2 + 2*reg_count) on success, 0 on failure; *err set to SQ_MB_*. */
size_t modbus_read_raw(const sq_modbus_t *mb, uint8_t slave, uint8_t func,
                       uint16_t reg_start, uint16_t reg_count,
                       uint8_t *out, size_t cap, uint8_t *err);
