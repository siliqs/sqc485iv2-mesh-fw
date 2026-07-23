/* hal_serial.h — RS485 / Modbus line port. Frozen contract (HAL_INTERFACE.md §1.1). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum { HAL_PARITY_NONE, HAL_PARITY_EVEN, HAL_PARITY_ODD } hal_parity_t;

bool hal_serial_init(uint32_t baud, hal_parity_t parity, uint8_t stop_bits);
void hal_serial_set_tx(bool enable);                              /* drive RS485 DE; polarity from BOARD */
int  hal_serial_write(const uint8_t *buf, size_t len);           /* bytes written, <0 err */
int  hal_serial_read (uint8_t *buf, size_t len, uint32_t to_ms); /* bytes read, 0 timeout */
void hal_serial_flush(void);
