/* config.c — defaults, load/save via hal_store, schema versioning, type helpers. */
#include <string.h>
#include "config.h"
#include "modbus.h"          /* modbus_crc16 for the blob CRC */
#include "hal/hal_store.h"

#define CONFIG_KEY "cfg"
#define SQ_BLOB_HDR 37       /* fixed bytes before polls[]; polls start here */

const char *sq_type_name(sq_datatype_t t)
{
    switch (t) {
    case SQ_TYPE_RAW: return "RAW";
    case SQ_TYPE_U16: return "U16";
    case SQ_TYPE_I16: return "I16";
    case SQ_TYPE_U32: return "U32";
    case SQ_TYPE_I32: return "I32";
    case SQ_TYPE_F32: return "F32";
    default:          return "?";
    }
}

bool sq_type_parse(const char *s, sq_datatype_t *out)
{
    if      (!strcmp(s, "raw")) *out = SQ_TYPE_RAW;
    else if (!strcmp(s, "u16")) *out = SQ_TYPE_U16;
    else if (!strcmp(s, "i16")) *out = SQ_TYPE_I16;
    else if (!strcmp(s, "u32")) *out = SQ_TYPE_U32;
    else if (!strcmp(s, "i32")) *out = SQ_TYPE_I32;
    else if (!strcmp(s, "f32")) *out = SQ_TYPE_F32;
    else return false;
    return true;
}

void config_set_defaults(sq_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->schema_version = SQ_CONFIG_VERSION;
    strncpy(c->device_name, "SQC485I", SQ_NAME_LEN - 1);

    memset(c->lorawan.dev_eui, 0x55, 8);
    memset(c->lorawan.join_eui, 0x00, 8);
    memset(c->lorawan.app_key, 0x55, 16);
    c->lorawan.adr       = true;
    c->lorawan.dr        = 5;
    c->lorawan.confirmed = false;
    c->lorawan.nb_trans  = 1;

    c->modbus.baud                = 9600;
    c->modbus.parity              = HAL_PARITY_NONE;
    c->modbus.stop_bits           = 1;
    c->modbus.response_timeout_ms = 1000;
    c->modbus.retries             = 3;
    c->modbus.poll_gap_ms         = 20;

    /* Example datapoints:
       poll[0] RAW (transparent passthrough) — cloud forwards opaque bytes.
       poll[1] typed metadata — cloud would decode as i16 ×0.1 (temperature).
       poll[2] an absent slave — demonstrates the fixed-length error frame so
               byte boundaries stay aligned for the cloud decoder. */
    c->poll_count = 3;

    strncpy(c->polls[0].name, "plc_block", SQ_NAME_LEN - 1);
    c->polls[0].slave = 1; c->polls[0].function = 3;
    c->polls[0].reg_start = 0; c->polls[0].reg_count = 2;
    c->polls[0].type = SQ_TYPE_RAW;          /* passthrough (default) */
    c->polls[0].scale = 1.0f;

    strncpy(c->polls[1].name, "temp", SQ_NAME_LEN - 1);
    c->polls[1].slave = 1; c->polls[1].function = 3;
    c->polls[1].reg_start = 8; c->polls[1].reg_count = 1;
    c->polls[1].type = SQ_TYPE_I16;          /* cloud metadata only */
    c->polls[1].scale = 0.1f;

    strncpy(c->polls[2].name, "absent", SQ_NAME_LEN - 1);
    c->polls[2].slave = 9; c->polls[2].function = 3;   /* no such slave -> error frame */
    c->polls[2].reg_start = 0; c->polls[2].reg_count = 1;
    c->polls[2].type = SQ_TYPE_RAW;
    c->polls[2].scale = 1.0f;

    c->power.uplink_interval_s = 60;
    c->power.deep_sleep        = true;

    c->tx.dest_node = 0;   /* broadcast */
    c->tx.channel   = 0;   /* primary */
    c->tx.confirmed = false;
    c->rs485_enabled = true;

    c->tunnel.enabled     = false;
    c->tunnel.peer_node   = 0;
    c->tunnel.idle_gap_ms = 20;   /* ~end-of-frame silence on the local bus */
}

bool config_load(sq_config_t *c)
{
    sq_config_t tmp;
    int n = hal_store_get(CONFIG_KEY, &tmp, sizeof(tmp));
    if (n == (int)sizeof(tmp) && tmp.schema_version == SQ_CONFIG_VERSION) {
        *c = tmp;
        return true;
    }
    config_set_defaults(c);
    return false;
}

bool config_save(const sq_config_t *c)
{
    if (!hal_store_set(CONFIG_KEY, c, sizeof(*c))) return false;
    return hal_store_commit();
}

bool config_factory_reset(sq_config_t *c)
{
    config_set_defaults(c);
    return config_save(c);
}

/* ── Binary config blob (device download path) ────────────────────────────── */

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put_u32(uint8_t *p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

size_t config_to_blob(const sq_config_t *c, uint8_t *out, size_t cap)
{
    /* v3 appends tx routing (dest_node u32 + channel u8 = 5 bytes); v4 then appends
       the tunnel block (peer_node u32 + idle_gap u16 = 6 bytes) after the polls. */
    size_t need = SQ_BLOB_HDR + (size_t)c->poll_count * 6 + 5 + 6 + 2;
    if (need > cap) return 0;

    out[0] = 'S'; out[1] = 'Q'; out[2] = SQ_CONFIG_VERSION & 0xFF;
    out[3] = (c->rs485_enabled ? 0 : SQ_FLAG_RS485_OFF)      /* flags */
           | (c->tunnel.enabled ? SQ_FLAG_TUNNEL_ON : 0)
           | (c->tx.confirmed   ? SQ_FLAG_CONFIRMED : 0);
    memset(out + 4, 0, 16);
    for (int i = 0; i < 16 && c->device_name[i]; i++) out[4 + i] = (uint8_t)c->device_name[i];
    put_u32(out + 20, c->modbus.baud);
    out[24] = (uint8_t)c->modbus.parity;
    out[25] = c->modbus.stop_bits;
    put_u16(out + 26, c->modbus.response_timeout_ms);
    out[28] = c->modbus.retries;
    put_u16(out + 29, c->modbus.poll_gap_ms);
    put_u32(out + 31, c->power.uplink_interval_s);
    out[35] = c->power.deep_sleep ? 1 : 0;
    out[36] = c->poll_count;
    for (uint8_t i = 0; i < c->poll_count; i++) {
        uint8_t *q = out + SQ_BLOB_HDR + i * 6;
        const sq_poll_t *p = &c->polls[i];
        q[0] = p->slave; q[1] = p->function;
        put_u16(q + 2, p->reg_start);
        put_u16(q + 4, p->reg_count);
    }
    uint8_t *t = out + SQ_BLOB_HDR + (size_t)c->poll_count * 6;   /* tx routing (v3) */
    put_u32(t, c->tx.dest_node);
    t[4] = c->tx.channel;
    uint8_t *tn = t + 5;                                          /* tunnel block (v4) */
    put_u32(tn, c->tunnel.peer_node);
    put_u16(tn + 4, c->tunnel.idle_gap_ms);
    put_u16(out + need - 2, modbus_crc16(out, need - 2));
    return need;
}

bool config_from_blob(sq_config_t *c, const uint8_t *b, size_t len)
{
    if (len < SQ_BLOB_HDR + 2) return false;
    uint8_t ver = b[2];
    if (b[0] != 'S' || b[1] != 'Q' || (ver != 2 && ver != 3 && ver != 4)) return false;

    uint8_t pc = b[36];
    if (pc > SQ_MAX_POLLS) return false;
    size_t extra = (ver >= 3 ? 5 : 0) + (ver >= 4 ? 6 : 0);   /* v3 tx routing + v4 tunnel */
    size_t need = SQ_BLOB_HDR + (size_t)pc * 6 + extra + 2;
    if (len != need) return false;
    if (get_u16(b + need - 2) != modbus_crc16(b, need - 2)) return false;

    /* preserve c->lorawan; overwrite the deployment fields */
    c->rs485_enabled = (b[3] & SQ_FLAG_RS485_OFF) ? false : true;
    c->tx.confirmed  = (b[3] & SQ_FLAG_CONFIRMED) ? true : false;
    memset(c->device_name, 0, SQ_NAME_LEN);
    memcpy(c->device_name, b + 4, 16);
    c->device_name[SQ_NAME_LEN - 1] = 0;
    c->modbus.baud                = get_u32(b + 20);
    c->modbus.parity              = (hal_parity_t)b[24];
    c->modbus.stop_bits           = b[25];
    c->modbus.response_timeout_ms = get_u16(b + 26);
    c->modbus.retries             = b[28];
    c->modbus.poll_gap_ms         = get_u16(b + 29);
    c->power.uplink_interval_s    = get_u32(b + 31);
    c->power.deep_sleep           = b[35] ? true : false;
    c->poll_count                 = pc;
    for (uint8_t i = 0; i < pc; i++) {
        const uint8_t *q = b + SQ_BLOB_HDR + i * 6;
        sq_poll_t *p = &c->polls[i];
        memset(p, 0, sizeof(*p));
        p->slave     = q[0];
        p->function  = q[1];
        p->reg_start = get_u16(q + 2);
        p->reg_count = get_u16(q + 4);
        p->type      = SQ_TYPE_RAW;   /* device read-plan; real type is cloud-side */
        p->scale     = 1.0f;
    }
    if (ver >= 3) {
        const uint8_t *t = b + SQ_BLOB_HDR + (size_t)pc * 6;
        c->tx.dest_node = get_u32(t);
        c->tx.channel   = t[4];
    } else {
        c->tx.dest_node = 0;          /* v2 blob: broadcast on primary */
        c->tx.channel   = 0;
    }
    if (ver >= 4) {
        const uint8_t *tn = b + SQ_BLOB_HDR + (size_t)pc * 6 + 5;
        c->tunnel.enabled     = (b[3] & SQ_FLAG_TUNNEL_ON) ? true : false;
        c->tunnel.peer_node   = get_u32(tn);
        c->tunnel.idle_gap_ms = get_u16(tn + 4);
    } else {
        c->tunnel.enabled     = false;   /* v2/v3 blob: no tunnel */
        c->tunnel.peer_node   = 0;
        c->tunnel.idle_gap_ms = 20;
    }
    c->schema_version = SQ_CONFIG_VERSION;
    return true;
}
