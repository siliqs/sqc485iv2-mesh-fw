/* board_profile.h — per-hardware-rev data (HAL_INTERFACE.md §2). Not code: a table. */
#pragma once
#include <stdbool.h>

typedef struct {
    const char *name;             /* e.g. "SQC485I-v233" */
    bool rs485_de_inverted;       /* DE polarity */
    bool rs485_tx_echo;           /* /RE tied low -> RX echoes TX; app strips it */
    bool rs485_power_switched;    /* transceiver fed via Vext; app powers it per poll */
    bool has_ble;
} board_profile_t;

extern const board_profile_t BOARD;
