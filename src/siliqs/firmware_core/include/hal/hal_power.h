/* hal_power.h / hal_led.h — power + LED ports. Frozen contract (HAL_INTERFACE.md §1.6). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

uint16_t hal_supply_mv(void);   /* battery / input rail, millivolts */
void     hal_led_set(bool on);
