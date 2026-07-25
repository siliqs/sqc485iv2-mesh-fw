/* poll.c — execute the poll list, concatenate raw Modbus responses.
   Each poll contributes a DETERMINISTIC number of bytes (2 + 2*reg_count) so the
   cloud can slice the concatenated payload by config. A failed read emits a
   same-length error frame instead of being dropped — dropping would shift every
   following byte and corrupt the cloud decode. */
#include "poll.h"
#include "hal/hal_time.h"
#include "modbus.h"

size_t poll_collect_raw(const sq_config_t *cfg, uint8_t *out, size_t cap)
{
    size_t len = 0;
    for (uint8_t i = 0; i < cfg->poll_count; i++) {
        const sq_poll_t *p = &cfg->polls[i];

        size_t outlen = 2u + 2u * (size_t)p->reg_count; /* [addr][func][data] */
        if (len + outlen > cap)
            break; /* stop on a poll boundary */

        uint8_t err, exc;
        size_t n =
            modbus_read_raw(&cfg->modbus, p->slave, p->function, p->reg_start, p->reg_count, out + len, cap - len, &err, &exc);
        if (n == 0) {
            /* Error frame, same length: [slave][func|0x80][err ...]. The 0x80 bit
               (Modbus exception convention) flags the failure; the length matches
               a success frame so the cloud's config-based slicing stays aligned. */
            out[len + 0] = p->slave;
            out[len + 1] = (uint8_t)(p->function | 0x80);
            for (size_t k = 2; k < outlen; k++)
                out[len + k] = err;
            /* When the slave REFUSED, byte 3 carries its reason instead of a
               repeat of ours: "register absent" and "device busy" call for very
               different action in the field, and the padding was pure redundancy.
               Safe to add — SQ_MB_ERR_EXCEPTION was unreachable until the reply
               length was read in stages, so no deployed decoder has ever seen an
               error frame carrying it, and every other error frame is unchanged.
               reg_count >= 1 guarantees outlen >= 4, so byte 3 always exists. */
            if (err == SQ_MB_ERR_EXCEPTION)
                out[len + 3] = exc;
            n = outlen;
        }
        len += n;

        if (i + 1 < cfg->poll_count && cfg->modbus.poll_gap_ms)
            hal_delay(cfg->modbus.poll_gap_ms); /* let the RS485 bus settle */
    }
    return len;
}
