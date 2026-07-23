/* hal_link.h — provisioning transport port (BLE and USB-C interchangeable).
   Frozen contract (HAL_INTERFACE.md §1.5). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int  (*read )(uint8_t *buf, size_t len, uint32_t to_ms); /* bytes, 0 timeout */
    int  (*write)(const uint8_t *buf, size_t len);           /* bytes written */
    bool (*connected)(void);
} hal_link_t;

hal_link_t *hal_link_ble(void);   /* NULL if board has no BLE */
hal_link_t *hal_link_usb(void);
