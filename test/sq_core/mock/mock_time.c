/*
 * mock_time.c — virtual clock.
 *
 * hal_delay() advances the clock instead of sleeping, so a test can assert that
 * poll.c actually waited poll_gap_ms between polls and that modbus.c burned
 * response_timeout_ms on every retry — without the suite taking seconds.
 */
#include "hal/hal_time.h"
#include "mock_hal.h"

static uint32_t s_now;

uint32_t hal_millis(void)
{
    return s_now;
}

void hal_delay(uint32_t ms)
{
    s_now += ms;
}

void hal_deep_sleep(uint32_t ms)
{
    s_now += ms; /* on device this reboots; here it is just elapsed time */
}

void hal_watchdog_set(uint32_t ms)
{
    (void)ms;
}

void hal_watchdog_feed(void) {}

void mock_time_reset(void)
{
    s_now = 0;
}

uint32_t mock_time_now(void)
{
    return s_now;
}

void mock_time_advance(uint32_t ms)
{
    s_now += ms;
}
