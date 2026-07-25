/* modbus.h — portable Modbus-RTU master engine (raw-forward).
   Reusable verbatim in both firmware versions (Star app core + Meshtastic module). */
#pragma once
#include "config.h"
#include <stddef.h>
#include <stdint.h>

enum {
    SQ_MB_OK = 0,
    SQ_MB_ERR_TIMEOUT = 1,   /* nothing (or not enough) came back in time     */
    SQ_MB_ERR_CRC = 2,       /* frame arrived corrupted                       */
    SQ_MB_ERR_EXCEPTION = 3, /* the slave answered, refusing the request      */
    SQ_MB_ERR_SHORT = 4,     /* bad byte count, or a request we cannot make   */
    SQ_MB_ERR_MISMATCH = 5   /* someone else's reply, or the wrong function   */
};

/* [addr][func|0x80][code][crc][crc] — an exception is this length whatever was
   asked for, which is why the reply length cannot be assumed up front. */
#define SQ_MB_EXCEPTION_FRAME_LEN 5

/* Exception codes, MODBUS Application Protocol V1.1b3 §7. The spec itself splits
   these into "your request is wrong" and "ask me again later", which is what
   decides whether retrying is worth the bus time. */
#define SQ_MB_EXC_ILLEGAL_FUNCTION 0x01
#define SQ_MB_EXC_ILLEGAL_ADDRESS 0x02
#define SQ_MB_EXC_ILLEGAL_VALUE 0x03
#define SQ_MB_EXC_DEVICE_FAILURE 0x04
#define SQ_MB_EXC_ACKNOWLEDGE 0x05
#define SQ_MB_EXC_DEVICE_BUSY 0x06
#define SQ_MB_EXC_MEMORY_PARITY 0x08
#define SQ_MB_EXC_GATEWAY_PATH 0x0A
#define SQ_MB_EXC_GATEWAY_NO_RESPONSE 0x0B

/* True when repeating the identical request could plausibly succeed.

   A slave that answers "illegal data address" will answer it again no matter how
   many times it is asked — that is a wrong poll plan, and retrying only spends
   bus time and, on a battery node, wake time. "Device busy" or "acknowledge"
   mean the opposite: the request was understood and the slave wants to be asked
   again. Codes this firmware does not recognise are treated as transient, so an
   unfamiliar slave still gets the benefit of the retry budget. */
bool modbus_exception_is_transient(uint8_t exception_code);

/* Standard Modbus RTU CRC16. */
uint16_t modbus_crc16(const uint8_t *buf, size_t len);

/* Do one read transaction and return the RAW forward bytes: [addr][func][data...]
   (bytecount and CRC dropped — the raw-forward wire format). Handles DE,
   TX-echo strip, CRC validation and retries. Returns the byte count written to
   out (= 2 + 2*reg_count) on success, 0 on failure; *err set to SQ_MB_*. */
size_t modbus_read_raw(const sq_modbus_t *mb, uint8_t slave, uint8_t func, uint16_t reg_start, uint16_t reg_count, uint8_t *out,
                       size_t cap, uint8_t *err);
