/*
 * mock_board.c — writable BOARD storage.
 *
 * Intentionally includes mock_board_layout.h rather than board_profile.h; see
 * that header for why. Nothing else belongs in this file — the accessors live in
 * mock_board_check.c, which can safely see both type definitions.
 */
#include "mock_board_layout.h"

mock_board_t BOARD = {
    .name = "MOCK-v233",
    .rs485_de_inverted = true,
    .rs485_tx_echo = true,
    .rs485_power_switched = false,
    .has_ble = true,
};

void *mock_board_raw(void)
{
    return &BOARD;
}
