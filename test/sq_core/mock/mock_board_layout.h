/*
 * mock_board_layout.h — a deliberate duplicate of board_profile_t.
 *
 * Tests need to flip BOARD.rs485_tx_echo at runtime, but board_profile.h
 * declares BOARD const, and writing through a cast-away-const pointer to a
 * genuinely const object is undefined (on Darwin it lands in a read-only page).
 * __attribute__((alias)) would solve it on Linux but is unsupported on Darwin.
 *
 * So the storage is defined in mock_board.c, which includes THIS header instead
 * of the real one, making BOARD an ordinary writable object. mock_board_check.c
 * includes both headers and static-asserts that the two layouts still agree, so
 * a field added to board_profile_t is a compile error here rather than silent
 * memory corruption.
 */
#pragma once

#include <stdbool.h>

typedef struct {
    const char *name;
    bool rs485_de_inverted;
    bool rs485_tx_echo;
    bool rs485_power_switched;
    bool has_ble;
} mock_board_t;

void *mock_board_raw(void);
