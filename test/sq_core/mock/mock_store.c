/*
 * mock_store.c — in-memory key/value store standing in for NVS / LittleFS.
 *
 * Adapted from 14_SQC485Iv2_basic/firmware_core/stub/stub_store.c, plus a
 * raw-write hook so a test can plant a record of the WRONG size — that is how a
 * device that was flashed with an older sq_config_t layout looks to config_load().
 */
#include <string.h>

#include "hal/hal_store.h"
#include "mock_hal.h"

#define MAX_KEYS 8
#define MAX_VAL 1024

static struct {
    char key[24];
    uint8_t val[MAX_VAL];
    size_t len;
    bool used;
} s_kv[MAX_KEYS];

static int find(const char *key)
{
    for (int i = 0; i < MAX_KEYS; i++)
        if (s_kv[i].used && strcmp(s_kv[i].key, key) == 0)
            return i;
    return -1;
}

bool hal_store_init(void)
{
    memset(s_kv, 0, sizeof(s_kv));
    return true;
}

int hal_store_get(const char *key, void *out, size_t max_len)
{
    int i = find(key);
    if (i < 0)
        return -1;
    size_t n = (s_kv[i].len < max_len) ? s_kv[i].len : max_len;
    memcpy(out, s_kv[i].val, n);
    return (int)s_kv[i].len;
}

bool hal_store_set(const char *key, const void *data, size_t len)
{
    if (len > MAX_VAL)
        return false;
    int i = find(key);
    if (i < 0)
        for (int j = 0; j < MAX_KEYS && i < 0; j++)
            if (!s_kv[j].used)
                i = j;
    if (i < 0)
        return false;
    s_kv[i].used = true;
    strncpy(s_kv[i].key, key, sizeof(s_kv[i].key) - 1);
    memcpy(s_kv[i].val, data, len);
    s_kv[i].len = len;
    return true;
}

bool hal_store_erase(const char *key)
{
    int i = find(key);
    if (i >= 0)
        s_kv[i].used = false;
    return true;
}

bool hal_store_commit(void)
{
    return true;
}

/* ── test control ─────────────────────────────────────────────────────────── */

void mock_store_reset(void)
{
    memset(s_kv, 0, sizeof(s_kv));
}

void mock_store_put_raw(const char *key, const void *data, size_t len)
{
    hal_store_set(key, data, len);
}

int mock_store_len(const char *key)
{
    int i = find(key);
    return i < 0 ? -1 : (int)s_kv[i].len;
}
