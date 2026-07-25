/*
 * mock_hal.h — test control surface for the firmware_core HAL mocks.
 *
 * The 14_SQC485Iv2_basic stub HAL simulates a *working* meter so the demo can
 * run end to end. These mocks go further: the RS485 line is scriptable per
 * attempt (so retry / CRC / timeout paths are reachable), time is virtual (so
 * poll gaps and retry delays are asserted rather than slept through), and every
 * DE/write/read call is logged (so RS485 turnaround ordering is a test, not a
 * code review).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_profile.h"

/* ── board profile ────────────────────────────────────────────────────────── */
/* Writable view of BOARD so a test can flip rs485_tx_echo / DE polarity. */
board_profile_t *mock_board(void);
void mock_board_reset(void); /* v233 profile: DE inverted, TX echo on */

/* ── virtual clock ────────────────────────────────────────────────────────── */
void mock_time_reset(void);
uint32_t mock_time_now(void);         /* ms since mock_time_reset() */
void mock_time_advance(uint32_t ms);  /* used by the serial mock to charge timeouts */

/* ── RS485 line ───────────────────────────────────────────────────────────── */

/* How the simulated slave answers ONE request. */
typedef enum {
    MOCK_RSP_GOOD = 0,      /* well-formed RTU response from the register file */
    MOCK_RSP_SILENT,        /* no answer at all -> engine must time out        */
    MOCK_RSP_BAD_CRC,       /* correct frame, last CRC byte flipped            */
    MOCK_RSP_BAD_BYTECOUNT, /* byte-count field does not match 2*reg_count     */
    MOCK_RSP_WRONG_SLAVE,   /* answer carries a different slave address        */
    MOCK_RSP_WRONG_FUNC,    /* answer carries a different function code        */
    MOCK_RSP_EXCEPTION      /* 5-byte Modbus exception frame (func|0x80, code) */
} mock_rsp_t;

void mock_serial_reset(void);

/* Every request gets the same answer. */
void mock_serial_set_mode(mock_rsp_t mode);

/* Script consecutive attempts: attempt i is answered with modes[i]; attempts
   past the end reuse the LAST entry. Lets a test say "first read is corrupt,
   the retry succeeds". */
void mock_serial_script(const mock_rsp_t *modes, size_t n);

/* Register file the simulated slave reads from (default: 1000 + addr). */
void mock_serial_set_reg(uint16_t addr, uint16_t value);

/* Exception code returned in MOCK_RSP_EXCEPTION frames (default 0x02). */
void mock_serial_set_exception_code(uint8_t code);

/* Captured transmissions. */
size_t mock_serial_tx_count(void);                    /* number of hal_serial_write() calls */
const uint8_t *mock_serial_tx(size_t i, size_t *len); /* NULL when i is out of range */

/* Call log, one character per HAL call, in order:
     '+' set_tx(true)   '-' set_tx(false)   'W' write   'F' flush   'R' read
   A correct Modbus turnaround therefore starts "+WF-R". */
const char *mock_serial_log(void);

/* ── persistent store ─────────────────────────────────────────────────────── */
void mock_store_reset(void);
/* Force a raw blob under a key — used to fake a record written by an older
   firmware whose sq_config_t had a different size. */
void mock_store_put_raw(const char *key, const void *data, size_t len);
int mock_store_len(const char *key); /* -1 when absent */
