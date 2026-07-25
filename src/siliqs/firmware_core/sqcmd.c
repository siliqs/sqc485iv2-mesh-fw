/* sqcmd.c — SQ command classification and reply construction. */
#include <string.h>

#include "sqcmd.h"

sq_cmd_t sq_classify(const uint8_t *b, size_t n)
{
    if (n < 3 || b[0] != 'S' || b[1] != 'Q')
        return SQ_CMD_IGNORE;

    /* Three-byte commands. */
    switch (b[2]) {
    case '?':
        return SQ_CMD_POLL_NOW;
    case '>':
        return SQ_CMD_RS485_BRIDGE;
    case '}':
        return SQ_CMD_TUNNEL_FORWARD;
    case '{':
        return SQ_CMD_TUNNEL_REPLY;
    case '~':
        return SQ_CMD_PIPE_DATA;
    default:
        break;
    }

    if (n < 4)
        return SQ_CMD_IGNORE;

    /* Four-byte commands. The trailing '?' is what separates a query from the
       reply to it, so a node never acts on another node's answer. */
    if (b[2] == 'P')
        return SQ_CMD_BLE_POWER; /* byte 3 is the dBm value, not a marker */
    if (b[2] == 'V' && b[3] == '?')
        return SQ_CMD_CAPABILITY;
    if (b[2] == 'G' && b[3] == '?')
        return SQ_CMD_GET_CONFIG;

    /* Only a small integer at byte 2 is a config-blob version. */
    if (b[2] >= 2 && b[2] <= 15)
        return SQ_CMD_CONFIG_BLOB;

    /* Everything else 'SQ*' is a command or reply marker — all printable ASCII,
       so >= 0x20 — including our own 'SQ<' / 'SQ!' / 'SQV…' / 'SQG…' replies.
       Treating those as a blob would emit a NAK for every reply we send, and
       that loops over the air. */
    if (b[2] >= 0x20)
        return SQ_CMD_OWN_REPLY;

    return SQ_CMD_IGNORE; /* a version we do not know, or a malformed frame */
}

uint8_t sq_bridge_reply_marker(sq_cmd_t cmd)
{
    switch (cmd) {
    case SQ_CMD_RS485_BRIDGE:
        return '<'; /* the USB tool / configurator */
    case SQ_CMD_TUNNEL_FORWARD:
        return '{'; /* the autonomous tunnel master */
    default:
        return 0;
    }
}

void sq_parse_bridge_request(const uint8_t *body, size_t n, const sq_modbus_t *link, sq_bridge_req_t *out)
{
    if (n >= SQ_BRIDGE_HEADER_LEN) {
        out->baud = (uint32_t)body[0] | ((uint32_t)body[1] << 8) | ((uint32_t)body[2] << 16) | ((uint32_t)body[3] << 24);
        out->parity = body[4];
        out->stop_bits = body[5] ? body[5] : 1;
        out->frame = body + SQ_BRIDGE_HEADER_LEN;
        out->frame_len = n - SQ_BRIDGE_HEADER_LEN;
        out->had_header = true;
    } else {
        out->baud = link->baud;
        out->parity = (uint8_t)link->parity;
        out->stop_bits = link->stop_bits;
        out->frame = body;
        out->frame_len = n;
        out->had_header = false;
    }
}

size_t sq_build_capability_reply(uint8_t *out, size_t cap)
{
    const char *fw = SQ_FW_VERSION;
    const char *pid = SQ_PRODUCT_ID;
    size_t fl = strlen(fw);
    size_t pdl = strlen(pid);
    size_t need = 7 + fl + 1 + pdl;

    if (need > cap)
        return 0;

    size_t i = 0;
    out[i++] = 'S';
    out[i++] = 'Q';
    out[i++] = 'V';
    out[i++] = SQ_CAP_PROTO;
    out[i++] = SQ_CONFIG_VERSION;
    out[i++] = SQ_FEATURES;
    out[i++] = (uint8_t)fl;
    memcpy(out + i, fw, fl);
    i += fl;
    out[i++] = (uint8_t)pdl; /* product id appended from proto 2 onwards */
    memcpy(out + i, pid, pdl);
    i += pdl;
    return i;
}

size_t sq_build_get_config_reply(const sq_config_t *cfg, uint8_t *out, size_t cap)
{
    if (cap < 3)
        return 0;
    out[0] = 'S';
    out[1] = 'Q';
    out[2] = 'G';
    size_t bl = config_to_blob(cfg, out + 3, cap - 3);
    return bl ? 3 + bl : 0;
}

size_t sq_plan_payload_len(const sq_config_t *cfg)
{
    size_t len = 0;
    for (uint8_t i = 0; i < cfg->poll_count && i < SQ_MAX_POLLS; i++)
        len += 2u + 2u * (size_t)cfg->polls[i].reg_count;
    return len;
}

size_t sq_build_config_ack(uint8_t status, uint8_t *out, size_t cap)
{
    if (cap < SQ_CONFIG_ACK_LEN)
        return 0;
    out[0] = 'S';
    out[1] = 'Q';
    out[2] = '!';
    out[3] = status;
    return SQ_CONFIG_ACK_LEN;
}
