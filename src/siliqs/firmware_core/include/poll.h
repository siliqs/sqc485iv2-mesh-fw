/* poll.h — run the poll list and build the raw-forward payload. */
#pragma once
#include "config.h"
#include <stddef.h>
#include <stdint.h>

/* Run every poll in cfg->polls[] in order; append each [addr][func][data] to out.
   Returns total bytes written (<= cap).

   A failed read is NOT skipped — it emits a fixed-length error frame so the
   cloud's config-based slicing stays aligned:

       [slave] [function|0x80] [sq_err] [sq_err …]

   with one exception, literally: when sq_err is SQ_MB_ERR_EXCEPTION the slave
   answered and refused, and byte 3 carries ITS Modbus exception code rather than
   another copy of ours — 0x02 "register absent" means fix the poll plan, 0x06
   "device busy" means look at the device. */
size_t poll_collect_raw(const sq_config_t *cfg, uint8_t *out, size_t cap);
