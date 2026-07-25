/*
 * mock_board_check.c — proves the duplicated board layout still matches the real
 * one, and exposes the writable storage as a board_profile_t.
 *
 * This is the only translation unit that sees both definitions. If someone adds
 * a field to board_profile_t, the static assertions below fail at compile time —
 * which is the entire reason the duplicate is allowed to exist.
 */
#include <stddef.h>

#include "board_profile.h"
#include "mock_board_layout.h"
#include "mock_hal.h"

_Static_assert(sizeof(mock_board_t) == sizeof(board_profile_t), "board_profile_t changed size — update mock_board_layout.h");
_Static_assert(_Alignof(mock_board_t) == _Alignof(board_profile_t),
               "board_profile_t changed alignment — update mock_board_layout.h");
_Static_assert(offsetof(mock_board_t, name) == offsetof(board_profile_t, name),
               "board_profile_t.name moved — update mock_board_layout.h");
_Static_assert(offsetof(mock_board_t, rs485_de_inverted) == offsetof(board_profile_t, rs485_de_inverted),
               "board_profile_t.rs485_de_inverted moved — update mock_board_layout.h");
_Static_assert(offsetof(mock_board_t, rs485_tx_echo) == offsetof(board_profile_t, rs485_tx_echo),
               "board_profile_t.rs485_tx_echo moved — update mock_board_layout.h");
_Static_assert(offsetof(mock_board_t, rs485_power_switched) == offsetof(board_profile_t, rs485_power_switched),
               "board_profile_t.rs485_power_switched moved — update mock_board_layout.h");
_Static_assert(offsetof(mock_board_t, has_ble) == offsetof(board_profile_t, has_ble),
               "board_profile_t.has_ble moved — update mock_board_layout.h");

board_profile_t *mock_board(void)
{
    return (board_profile_t *)mock_board_raw();
}

/* The SQC485Iv2 v233 profile: DE inverted through U7, /RE tied low so the line
   echoes every transmission. */
void mock_board_reset(void)
{
    board_profile_t *b = mock_board();
    b->name = "MOCK-v233";
    b->rs485_de_inverted = true;
    b->rs485_tx_echo = true;
    b->rs485_power_switched = false;
    b->has_ble = true;
}
