/* gw_store.h — gateway's runtime cache of remote nodes' raw-forward telemetry.
   Execution-only (never persisted to config/NVS): populated each time the
   gateway receives a node's mesh uplink, queried each time the gateway's own
   Modbus-slave responder (modbus_slave.c) answers an external RTU master.
   Sized and keyed off sq_gateway_reply_t.map[] — see config.h for the
   transparent/aggregated split and why BOTH modes need the map. */
#pragma once
#include "config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One cell's data is exactly one poll's raw-forward payload: 2*reg_count
   bytes, reg_count <= SQ_MAX_REGS. */
#define GW_STORE_MAX_DATA_LEN (2u * SQ_MAX_REGS)

/* Store capacity: at most one cell per configured map row, so bounding it to
   the map's own row count also bounds the store — no separate limit needed. */
#define GW_STORE_MAX_ENTRIES SQ_MAX_GW_MAP_ENTRIES

void gw_store_reset(void);

/* Slice one node's raw-forward uplink (the concatenated
   [slave][func][data...] stream poll_collect_raw() produces on that node)
   into cells, using gw->map[] rows whose node_id matches to know where each
   poll's boundary falls — the stream itself carries no length markers.
   Rows for other node_ids are skipped when walking the map, but every row
   still costs mesh-plan-order bookkeeping, so the caller's map must list this
   node's polls in the SAME order that node itself polls them (see config.h /
   the operator-facing register-map note).

   A poll's error frame ([slave][func|0x80][err...]) is NOT cached — a
   transient read failure on the remote node leaves this cache's last-good
   value in place rather than clobbering it, so a brief hiccup on the far
   side doesn't turn into a wrong answer on this side (the far end still gets
   another shot at it on its own next poll cycle). The cache slot's
   last_seen_ms is left untouched too, so staleness reflects "last time we
   actually had good data," not "last time we heard from the node at all."

   Returns the number of cells written or refreshed (0 if node_id matches no
   row, or the stream was too short to contain even the first matching poll). */
size_t gw_store_put(const sq_gateway_reply_t *gw, uint32_t node_id, const uint8_t *raw, size_t raw_len, uint32_t now_ms);

/* Look up a cached cell by its own key (slave, func, reg_start) — the same
   triple the request arrived under, whether that's a remote node's own
   address (transparent) or one resolved via virt_reg_offset (aggregated);
   either way modbus_slave.c resolves node_id/src_* first via gw->map[] and
   queries the cache using the resolved source key, not the request's wire
   address. Only an EXACT reg_count match hits, since a cell holds exactly one
   poll's worth of bytes and partial/overlapping register reads were never
   part of this feature's scope. Copies the cached bytes into data_out (must
   be >= reg_count*2) and reports last_seen_ms for the caller's own staleness
   check (this module doesn't know staleness_threshold_s's meaning — that
   decision belongs to modbus_slave.c). */
bool gw_store_find(uint8_t slave, uint8_t func, uint16_t reg_start, uint16_t reg_count, uint8_t *data_out, size_t cap,
                    uint32_t *last_seen_ms);
