/* poll.h — run the poll list and build the raw-forward payload. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "config.h"

/* Run every poll in cfg->polls[] in order; append each [addr][func][data] to out.
   Returns total bytes written (<= cap). Failed reads are skipped. */
size_t poll_collect_raw(const sq_config_t *cfg, uint8_t *out, size_t cap);
