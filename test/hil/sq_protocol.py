"""
sq_protocol.py — the SQ command set as seen from the host.

Encoders and parsers for the private-portnum protocol implemented in
src/modules/ModbusModule.cpp. Written from that file's documented byte layouts;
keeping it independent of the firmware is what lets it detect a change.

The config blob codec is shared with the unit-test fixtures
(test/sq_core/golden/make_golden.py) so there is exactly one host-side
implementation of the wire format to keep honest.
"""

from __future__ import annotations

import pathlib
import struct
import sys
from dataclasses import dataclass, field

_GOLDEN = pathlib.Path(__file__).resolve().parents[1] / "sq_core" / "golden"
sys.path.insert(0, str(_GOLDEN))
from make_golden import FLAG_CONFIRMED, FLAG_RS485_OFF, FLAG_TUNNEL_ON, build as build_blob, crc16  # noqa: E402

# Private application PortNum — SILIQS_MODBUS_PORTNUM in ModbusModule.h.
PORTNUM = 256

# Commands (host -> node)
CMD_POLL_NOW = b"SQ?"
CMD_CAPABILITY = b"SQV?"
CMD_GET_CONFIG = b"SQG?"
CMD_BLE_POWER = b"SQP"  # + int8 dBm
CMD_RS485_BRIDGE = b"SQ>"  # + raw bytes to put on the bus

# Reply markers (node -> host)
RPL_CAPABILITY = b"SQV"
RPL_GET_CONFIG = b"SQG"
RPL_CONFIG_ACK = b"SQ!"
RPL_RS485_BRIDGE = b"SQ<"

CONFIG_STATUS = {
    0: "applied",
    1: "invalid (bad magic / version / length / CRC)",
    2: "valid but failed to persist",
    3: "valid but the poll plan cannot fit one packet",
}

# Bytes a mesh packet carries — meshtastic_Constants_DATA_PAYLOAD_LEN.
MESH_PAYLOAD_LEN = 233

# Modbus error codes padded into a failed poll's error frame (firmware_core/modbus.h)
MB_ERR = {
    0: "ok",
    1: "timeout",
    2: "crc",
    3: "exception",
    4: "short",
    5: "mismatch",
}


@dataclass
class Capability:
    """Reply to SQV? — what the configurator uses to label and gate a node."""

    proto: int
    max_blob_version: int
    features: int
    firmware: str
    product_id: str

    FEATURE_NAMES = {
        0x01: "tunnel",
        0x02: "ble_power",
        0x04: "rs485_term",
        0x08: "poll_now",
        0x10: "deep_sleep",
        0x20: "get_config",
    }

    def feature_list(self) -> list[str]:
        return [name for bit, name in sorted(self.FEATURE_NAMES.items()) if self.features & bit]


@dataclass
class Poll:
    slave: int
    function: int
    reg_start: int
    reg_count: int

    def as_tuple(self):
        return (self.slave, self.function, self.reg_start, self.reg_count)

    @property
    def payload_len(self) -> int:
        """Bytes this poll contributes to the raw-forward payload, pass or fail."""
        return 2 + 2 * self.reg_count


@dataclass
class Config:
    """The device-side view of a config blob (the read plan and link settings)."""

    version: int = 4
    name: str = "hil"
    polls: list[Poll] = field(default_factory=list)
    rs485_enabled: bool = True
    tunnel_enabled: bool = False
    confirmed: bool = False
    baud: int = 9600
    parity: int = 0
    stop_bits: int = 1
    response_timeout_ms: int = 1000
    retries: int = 3
    poll_gap_ms: int = 20
    uplink_interval_s: int = 60
    deep_sleep: bool = True
    tx_dest: int = 0
    tx_channel: int = 0
    tunnel_peer: int = 0
    tunnel_idle_gap: int = 20

    def to_blob(self) -> bytes:
        flags = 0
        if not self.rs485_enabled:
            flags |= FLAG_RS485_OFF
        if self.tunnel_enabled:
            flags |= FLAG_TUNNEL_ON
        if self.confirmed:
            flags |= FLAG_CONFIRMED
        return build_blob(
            self.version,
            self.name,
            [p.as_tuple() for p in self.polls],
            flags=flags,
            baud=self.baud,
            parity=self.parity,
            stop_bits=self.stop_bits,
            response_timeout_ms=self.response_timeout_ms,
            retries=self.retries,
            poll_gap_ms=self.poll_gap_ms,
            uplink_interval_s=self.uplink_interval_s,
            deep_sleep=self.deep_sleep,
            tx_dest=self.tx_dest,
            tx_channel=self.tx_channel,
            tunnel_peer=self.tunnel_peer,
            tunnel_idle_gap=self.tunnel_idle_gap,
        )

    @property
    def expected_payload_len(self) -> int:
        return sum(p.payload_len for p in self.polls)


def parse_blob(blob: bytes) -> Config:
    """Decode a config blob the way the firmware does, for read-back comparison."""
    if len(blob) < 39 or blob[0:2] != b"SQ":
        raise ValueError(f"not a config blob: {blob[:4].hex()}")
    version = blob[2]
    if version not in (2, 3, 4):
        raise ValueError(f"unsupported blob version {version}")

    poll_count = blob[36]
    extra = (5 if version >= 3 else 0) + (6 if version >= 4 else 0)
    need = 37 + poll_count * 6 + extra + 2
    if len(blob) != need:
        raise ValueError(f"blob length {len(blob)} != expected {need}")
    if struct.unpack_from("<H", blob, need - 2)[0] != crc16(blob[: need - 2]):
        raise ValueError("blob CRC mismatch")

    flags = blob[3]
    cfg = Config(
        version=version,
        name=blob[4:20].rstrip(b"\x00").decode("ascii", "replace"),
        rs485_enabled=not (flags & FLAG_RS485_OFF),
        tunnel_enabled=bool(flags & FLAG_TUNNEL_ON),
        confirmed=bool(flags & FLAG_CONFIRMED),
        baud=struct.unpack_from("<I", blob, 20)[0],
        parity=blob[24],
        stop_bits=blob[25],
        response_timeout_ms=struct.unpack_from("<H", blob, 26)[0],
        retries=blob[28],
        poll_gap_ms=struct.unpack_from("<H", blob, 29)[0],
        uplink_interval_s=struct.unpack_from("<I", blob, 31)[0],
        deep_sleep=bool(blob[35]),
    )
    for i in range(poll_count):
        slave, func, start, count = struct.unpack_from("<BBHH", blob, 37 + i * 6)
        cfg.polls.append(Poll(slave, func, start, count))
    if version >= 3:
        off = 37 + poll_count * 6
        cfg.tx_dest = struct.unpack_from("<I", blob, off)[0]
        cfg.tx_channel = blob[off + 4]
    if version >= 4:
        off = 37 + poll_count * 6 + 5
        cfg.tunnel_peer = struct.unpack_from("<I", blob, off)[0]
        cfg.tunnel_idle_gap = struct.unpack_from("<H", blob, off + 4)[0]
    return cfg


def bridge_request(frame: bytes, baud: int = 9600, parity: int = 0, stop_bits: int = 1) -> bytes:
    """Build an 'SQ>' raw-bridge request: marker + link header + the bytes to send.

    The 6-byte link header is not optional in practice. rawBridge() treats ANY
    payload of six bytes or more as header-prefixed, and an 8-byte Modbus frame is
    the most natural thing to send — so a bare frame is silently reinterpreted:
    `01 03 00 00 00 02 C4 0B` becomes baud 0x00000301 (769), parity 0, stop 2, and
    a two-byte payload of `C4 0B`. The device then transmits garbage at 769 baud
    AND leaves its UART there, so every later poll fails too.
    """
    return CMD_RS485_BRIDGE + struct.pack("<IBB", baud, parity, stop_bits) + frame


def parse_capability(payload: bytes) -> Capability:
    """SQV + proto + max_blob_ver + features + fw_len + fw + pid_len + product_id."""
    if len(payload) < 7 or payload[0:3] != RPL_CAPABILITY:
        raise ValueError(f"not a capability reply: {payload[:4]!r}")
    proto, max_blob, features, fw_len = payload[3], payload[4], payload[5], payload[6]
    at = 7
    firmware = payload[at : at + fw_len].decode("ascii", "replace")
    at += fw_len

    product_id = ""
    if proto >= 2 and at < len(payload):
        pid_len = payload[at]
        at += 1
        product_id = payload[at : at + pid_len].decode("ascii", "replace")
    return Capability(proto, max_blob, features, firmware, product_id)


def split_raw_payload(payload: bytes, polls: list[Poll]):
    """Slice a raw-forward payload the way the cloud decoder does — by config alone.

    Returns one entry per poll: (poll, slave, function, data, error). ``error`` is
    None on success, otherwise the code padded into the fixed-length error frame.
    """
    out = []
    at = 0
    for poll in polls:
        chunk = payload[at : at + poll.payload_len]
        at += poll.payload_len
        if len(chunk) < 2:
            out.append((poll, None, None, b"", "truncated"))
            continue
        slave, func = chunk[0], chunk[1]
        data = chunk[2:]
        error = None
        if func & 0x80:
            code = data[0] if data else 0xFF
            error = MB_ERR.get(code, f"unknown({code})")
        out.append((poll, slave, func & 0x7F, data, error))
    return out
