/* hal_store.h — persistent key/value port (NVS-like). Frozen contract (HAL_INTERFACE.md §1.3). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

bool hal_store_init(void);
int  hal_store_get  (const char *key, void *out, size_t max_len); /* bytes, <0 if missing */
bool hal_store_set  (const char *key, const void *data, size_t len);
bool hal_store_erase(const char *key);
bool hal_store_commit(void);
