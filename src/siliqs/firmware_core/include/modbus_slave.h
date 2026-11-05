/* modbus_slave.h — gateway's passive Modbus RTU slave responder (Mesh version
   only). Listens on the LOCAL RS485 bus for an external master's read request
   and answers from gw_store's cached mesh telemetry — never by querying the
   mesh live (a mesh round-trip is seconds to tens of seconds, far past any
   Modbus master's own timeout). See config.h's sq_gateway_reply_t for the
   transparent/aggregated split this dispatches on. */
#pragma once
#include "config.h"
#include <stdbool.h>

/* Poll the RS485 line once. Uses T3.5 (MODBUS Application Protocol V1.1b3
   §2.5.1.1) inter-character silence to find a frame's end, the same framing
   any Modbus RTU slave uses. Returns true if a frame was received on the
   wire this call — whether or not it was addressed to one of the identities
   *cfg answers under, and whether or not it got a reply — false if the bus
   stayed idle for a full frame-gap with nothing arriving. No-ops (returns
   false) when neither transparent nor aggregated mode is enabled. */
bool modbus_slave_poll(const sq_config_t *cfg);
