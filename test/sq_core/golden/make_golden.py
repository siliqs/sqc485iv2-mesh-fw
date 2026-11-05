#!/usr/bin/env python3
"""
make_golden.py — generate the golden config-blob fixtures.

This is an INDEPENDENT implementation of the blob wire format, written from the
spec in firmware_core/include/config.h. It must never call the firmware code:
the whole point of a golden file is that a bug in config_to_blob() cannot quietly
become the expected answer.

Layout (little-endian), from config.h:

    off  size  field
      0     2  'S','Q'
      2     1  version (2, 3, 4 or 5)
      3     1  flags   (bit0 RS485_OFF, bit1 TUNNEL_ON, bit2 CONFIRMED)
      4    16  device_name, NUL padded
     20     4  baud
     24     1  parity  (0 none, 1 even, 2 odd)
     25     1  stop_bits
     26     2  response_timeout_ms
     28     1  retries
     29     2  poll_gap_ms
     31     4  uplink_interval_s
     35     1  deep_sleep
     36     1  poll_count
     37   6*n  polls: slave(1) function(1) reg_start(2) reg_count(2)
      -     5  v3+: tx dest_node(4) channel(1)
      -     6  v4+: tunnel peer_node(4) idle_gap_ms(2)
      -     7  v5+: gateway gw_flags(1) aggregated_slave_addr(1)
                    staleness_threshold_s(4) map_count(1)
      -  12*m  v5+: map[m]: node_id(4) src_slave(1) src_func(1)
                    src_reg_start(2) src_reg_count(2) virt_reg_offset(2)
      -     2  crc16 over everything before it

Regenerate with:  python3 make_golden.py
"""

import pathlib
import struct

HERE = pathlib.Path(__file__).parent

FLAG_RS485_OFF = 0x01
FLAG_TUNNEL_ON = 0x02
FLAG_CONFIRMED = 0x04

FLAG_GW_TRANSPARENT = 0x01
FLAG_GW_AGGREGATED = 0x02


def crc16(data: bytes) -> int:
    """Compute the standard Modbus RTU CRC16 (poly 0xA001, init 0xFFFF)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def build(
    version,
    name,
    polls,
    flags=0,
    baud=9600,
    parity=0,
    stop_bits=1,
    response_timeout_ms=1000,
    retries=3,
    poll_gap_ms=20,
    uplink_interval_s=60,
    deep_sleep=True,
    tx_dest=0,
    tx_channel=0,
    tunnel_peer=0,
    tunnel_idle_gap=20,
    gw_flags=0,
    gw_agg_slave=1,
    gw_staleness_s=3600,
    gw_map=(),
):
    body = bytearray()
    body += b"SQ"
    body += bytes([version, flags])
    body += name.encode("ascii")[:16].ljust(16, b"\x00")
    body += struct.pack("<I", baud)
    body += bytes([parity, stop_bits])
    body += struct.pack("<H", response_timeout_ms)
    body += bytes([retries])
    body += struct.pack("<H", poll_gap_ms)
    body += struct.pack("<I", uplink_interval_s)
    body += bytes([1 if deep_sleep else 0, len(polls)])
    for slave, func, reg_start, reg_count in polls:
        body += bytes([slave, func]) + struct.pack("<HH", reg_start, reg_count)
    if version >= 3:
        body += struct.pack("<I", tx_dest) + bytes([tx_channel])
    if version >= 4:
        body += struct.pack("<I", tunnel_peer) + struct.pack("<H", tunnel_idle_gap)
    if version >= 5:
        body += bytes([gw_flags, gw_agg_slave])
        body += struct.pack("<I", gw_staleness_s)
        body += bytes([len(gw_map)])
        for node_id, src_slave, src_func, src_reg_start, src_reg_count, virt_reg_offset in gw_map:
            body += struct.pack("<I", node_id)
            body += bytes([src_slave, src_func])
            body += struct.pack("<HHH", src_reg_start, src_reg_count, virt_reg_offset)
    body += struct.pack("<H", crc16(bytes(body)))
    return bytes(body)


# The exact blob config_set_defaults() + config_to_blob() must emit. Mirrors the
# defaults in firmware_core/config.c; a deliberate default change means updating
# this file, an accidental one means a failing test.
GOLDEN = {
    "v5_defaults": build(
        5,
        "SQC485I",
        [(1, 3, 0, 2), (1, 3, 8, 1), (9, 3, 0, 1)],
    ),
    # A v2 blob as an old configurator would still send it: no tx routing, no
    # tunnel block, RS485 disabled, every scalar deliberately non-default so a
    # field read from the wrong offset cannot pass by coincidence.
    "v2": build(
        2,
        "V2NODE",
        [(7, 4, 100, 10)],
        flags=FLAG_RS485_OFF,
        baud=19200,
        parity=2,
        stop_bits=2,
        response_timeout_ms=500,
        retries=5,
        poll_gap_ms=35,
        uplink_interval_s=300,
        deep_sleep=False,
    ),
    # v3 added tx routing (unicast + channel + confirmed flag).
    "v3": build(
        3,
        "V3NODE",
        [(2, 3, 40, 4), (3, 4, 0, 1)],
        flags=FLAG_CONFIRMED,
        baud=115200,
        parity=1,
        response_timeout_ms=250,
        retries=1,
        poll_gap_ms=5,
        uplink_interval_s=900,
        deep_sleep=True,
        tx_dest=0xDEADBEEF,
        tx_channel=3,
    ),
    # v4 added the RS485<->RS485 tunnel block.
    "v4": build(
        4,
        "V4NODE",
        [(5, 3, 1000, 16), (6, 4, 7, 2)],
        flags=FLAG_TUNNEL_ON | FLAG_CONFIRMED,
        baud=38400,
        parity=0,
        stop_bits=1,
        response_timeout_ms=1500,
        retries=2,
        poll_gap_ms=100,
        uplink_interval_s=30,
        deep_sleep=False,
        tx_dest=0x01020304,
        tx_channel=7,
        tunnel_peer=0x11223344,
        tunnel_idle_gap=50,
    ),
    # v5 added the gateway Modbus-slave reply block (transparent + aggregated,
    # staleness threshold, register map).
    "v5": build(
        5,
        "V5NODE",
        [(4, 3, 10, 2)],
        flags=FLAG_CONFIRMED,
        baud=57600,
        parity=2,
        response_timeout_ms=800,
        retries=2,
        poll_gap_ms=15,
        uplink_interval_s=120,
        deep_sleep=False,
        tx_dest=0xAABBCCDD,
        tx_channel=2,
        gw_flags=FLAG_GW_TRANSPARENT | FLAG_GW_AGGREGATED,
        gw_agg_slave=99,
        gw_staleness_s=1800,
        gw_map=[
            (0x81B8A03C, 1, 3, 0, 2, 0),
            (0x7D51BDB0, 5, 4, 100, 4, 2),
        ],
    ),
    # INVALID on purpose: 17 map entries, one past SQ_MAX_GW_MAP_ENTRIES, but
    # internally consistent (correct length, correct CRC) — proves the bounds
    # check rejects on map_count itself, the field that would overflow
    # gateway.map[].
    "v5_map_overflow": build(
        5,
        "GWOVERFLOW",
        [(1, 3, 0, 2)],
        gw_flags=FLAG_GW_AGGREGATED,
        gw_map=[(i, 1, 3, 0, 1, i) for i in range(17)],
    ),
    # Upper bound: SQ_MAX_POLLS entries, each at SQ_MAX_REGS registers.
    "v4_max": build(
        4,
        "MAXPOLLS",
        [(i + 1, 3, i * 16, 16) for i in range(8)],
        tx_dest=0xFFFFFFFF,
        tx_channel=7,
        tunnel_peer=0xFFFFFFFF,
        tunnel_idle_gap=65535,
    ),
    # INVALID on purpose: 9 polls, one past SQ_MAX_POLLS, but internally
    # consistent (correct length, correct CRC). Proves the bounds check rejects
    # it on the poll_count field itself and not as a side effect of a length or
    # checksum mismatch — this is the one that would overflow polls[].
    "v4_overflow": build(
        4,
        "OVERFLOW",
        [(i + 1, 3, i * 16, 2) for i in range(9)],
    ),
    # INVALID on purpose: uplink_interval_s = 0. Otherwise a perfectly formed blob
    # (correct length, correct CRC) — proves the bounds check rejects on the
    # interval field itself. 0 is the value that turns ModbusModule's runOnce()
    # into a busy loop (ModbusModule has no clamp of its own).
    "v4_interval_zero": build(
        4,
        "INTERVAL0",
        [(1, 3, 0, 2)],
        uplink_interval_s=0,
    ),
    # INVALID on purpose: uplink_interval_s past SQ_MAX_UPLINK_INTERVAL_S (604800)
    # but nowhere near the int32-ms overflow point (~2,147,483) — proves the upper
    # bound itself rejects, independent of the overflow it exists to stay clear of.
    "v4_interval_too_large": build(
        4,
        "INTERVALHI",
        [(1, 3, 0, 2)],
        uplink_interval_s=700000,
    ),
}

if __name__ == "__main__":
    for stem, blob in GOLDEN.items():
        path = HERE / f"blob_{stem}.hex"
        path.write_text(blob.hex() + "\n")
        print(f"{path.name:24s} {len(blob):3d} bytes  crc=0x{crc16(blob[:-2]):04X}")
