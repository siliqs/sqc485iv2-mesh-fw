/* hal_radio.h — LoRaWAN port. Frozen contract (HAL_INTERFACE.md §1.2). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint8_t dev_eui[8];
    uint8_t join_eui[8];
    uint8_t app_key[16];
} hal_lorawan_creds_t;                       /* region is AS923, fixed for the anchor release */

typedef struct { uint8_t fport; uint8_t len; uint8_t data[64]; } hal_downlink_t;

bool hal_radio_init(void);
bool hal_lorawan_join_otaa(const hal_lorawan_creds_t *c, uint32_t to_ms);
int  hal_lorawan_send(uint8_t fport, const uint8_t *data, size_t len, bool confirmed); /* 0 ok, <0 err */
bool hal_lorawan_get_downlink(hal_downlink_t *out);   /* true if one was received */
bool hal_radio_link_stats(int16_t *rssi_dbm, int8_t *snr_db);
