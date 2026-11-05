/* gw_store.c — see gw_store.h for the cache's role and the error-frame /
   node-poll-order rules this file relies on. */
#include "gw_store.h"

typedef struct {
    bool valid;
    uint8_t slave;
    uint8_t func;
    uint16_t reg_start;
    uint16_t reg_count;
    uint8_t data[GW_STORE_MAX_DATA_LEN];
    uint32_t last_seen_ms;
} gw_store_slot_t;

static gw_store_slot_t s_slots[GW_STORE_MAX_ENTRIES];

void gw_store_reset(void)
{
    for (size_t i = 0; i < GW_STORE_MAX_ENTRIES; i++)
        s_slots[i].valid = false;
}

/* Find the slot already keyed to (slave, func, reg_start), or an unused slot,
   or — the store is full and every slot is a different live key — the slot
   whose data is oldest. Store capacity is bounded by SQ_MAX_GW_MAP_ENTRIES
   (see gw_store.h), so eviction only matters if a caller reuses this module
   across a differently-shaped map without an intervening gw_store_reset(). */
static gw_store_slot_t *find_slot(uint8_t slave, uint8_t func, uint16_t reg_start)
{
    gw_store_slot_t *free_slot = NULL;
    gw_store_slot_t *oldest = NULL;
    for (size_t i = 0; i < GW_STORE_MAX_ENTRIES; i++) {
        gw_store_slot_t *s = &s_slots[i];
        if (s->valid && s->slave == slave && s->func == func && s->reg_start == reg_start)
            return s;
        if (!s->valid && !free_slot)
            free_slot = s;
        if (s->valid && (!oldest || s->last_seen_ms < oldest->last_seen_ms))
            oldest = s;
    }
    return free_slot ? free_slot : oldest;
}

size_t gw_store_put(const sq_gateway_reply_t *gw, uint32_t node_id, const uint8_t *raw, size_t raw_len, uint32_t now_ms)
{
    size_t written = 0;
    size_t cursor = 0;

    for (uint8_t i = 0; i < gw->map_count; i++) {
        const sq_gw_map_entry_t *row = &gw->map[i];
        if (row->node_id != node_id)
            continue; /* another node's row — doesn't consume this node's stream */

        size_t frame_len = 2u + 2u * (size_t)row->src_reg_count;
        if (cursor + frame_len > raw_len)
            break; /* short stream — matches poll_collect_raw()'s own truncate-on-boundary */

        const uint8_t *frame = raw + cursor;
        bool is_error = (frame[1] & 0x80) != 0;
        cursor += frame_len;

        if (is_error)
            continue; /* keep the last-good value; see gw_store.h */

        size_t data_len = frame_len - 2u;
        if (data_len > GW_STORE_MAX_DATA_LEN)
            continue; /* cannot happen while src_reg_count <= SQ_MAX_REGS, but stay in bounds */

        gw_store_slot_t *slot = find_slot(row->src_slave, row->src_func, row->src_reg_start);
        if (!slot)
            continue; /* GW_STORE_MAX_ENTRIES == SQ_MAX_GW_MAP_ENTRIES, so this cannot happen */

        slot->valid = true;
        slot->slave = row->src_slave;
        slot->func = row->src_func;
        slot->reg_start = row->src_reg_start;
        slot->reg_count = row->src_reg_count;
        for (size_t k = 0; k < data_len; k++)
            slot->data[k] = frame[2 + k];
        slot->last_seen_ms = now_ms;
        written++;
    }

    return written;
}

bool gw_store_find(uint8_t slave, uint8_t func, uint16_t reg_start, uint16_t reg_count, uint8_t *data_out, size_t cap,
                    uint32_t *last_seen_ms)
{
    for (size_t i = 0; i < GW_STORE_MAX_ENTRIES; i++) {
        gw_store_slot_t *s = &s_slots[i];
        if (!s->valid || s->slave != slave || s->func != func || s->reg_start != reg_start || s->reg_count != reg_count)
            continue;

        size_t data_len = 2u * (size_t)reg_count;
        if (data_len > cap)
            return false;

        for (size_t k = 0; k < data_len; k++)
            data_out[k] = s->data[k];
        if (last_seen_ms)
            *last_seen_ms = s->last_seen_ms;
        return true;
    }
    return false;
}
