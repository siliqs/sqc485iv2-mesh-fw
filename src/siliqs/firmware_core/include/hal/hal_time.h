/* hal_time.h — time, sleep, watchdog port. Frozen contract (HAL_INTERFACE.md §1.4). */
#pragma once
#include <stdint.h>

uint32_t hal_millis(void);
void     hal_delay(uint32_t ms);
void     hal_deep_sleep(uint32_t ms);   /* app treats return as a fresh boot */
void     hal_watchdog_set(uint32_t ms);
void     hal_watchdog_feed(void);
