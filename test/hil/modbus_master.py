"""
modbus_master.py — a minimal Modbus-RTU master driven from a USB-RS485 dongle.

The mirror image of modbus_slave.py: that file plays "fake slave" to test the
DUT's own master (the poll engine, firmware_core/modbus.c); this one plays
"fake master" to test the DUT's own slave (firmware_core/modbus_slave.c, the
gateway's passive T3.5 transparent/aggregated reply feature). Same dongle,
same wiring (test/hil/CLAUDE.md §1) — the poll engine and the gateway
responder are mutually exclusive on the DUT (ModbusModule.cpp::runOnce()),
so nothing needs to be reconfigured between the two roles beyond which end of
the wire is doing the asking.

Deliberately hand-rolled rather than pulled from pymodbus, same rationale as
modbus_slave.py: an independent implementation on this side of the wire is
what lets a shared misreading of the spec get caught instead of passing.
crc16() itself IS reused from modbus_slave.py rather than re-implemented here
— the "independent" boundary this repo cares about is not depending on
pymodbus, not which Python module owns the checksum, and every other script
in test/hil/ already imports it from there (fake_peer.py, qc_checks.py,
sq_hil.py).

Supports what modbus_slave.c actually dispatches: FC03 (read holding
registers) and FC04 (read input registers), 8-byte requests only.
"""

from __future__ import annotations

import time
from dataclasses import dataclass

import serial

from modbus_slave import PARITY_MAP, crc16


def build_request(slave: int, function: int, reg_start: int, reg_count: int) -> bytes:
    """FC03/FC04 read request — the only shape modbus_slave.c ever parses."""
    body = (
        bytes([slave, function])
        + reg_start.to_bytes(2, "big")
        + reg_count.to_bytes(2, "big")
    )
    return body + crc16(body).to_bytes(2, "little")


@dataclass
class Reply:
    """A parsed, CRC-verified response. `is_exception` picks which of
    `data`/`exception_code` is meaningful — mirrors the two shapes
    modbus_slave.c's send_reply()/send_exception() produce."""

    slave: int
    function: int
    is_exception: bool
    data: bytes = b""
    exception_code: int | None = None

    def registers(self, count: int) -> list[int]:
        """Decode a data reply's payload as big-endian 16-bit registers."""
        return [
            int.from_bytes(self.data[i : i + 2], "big") for i in range(0, count * 2, 2)
        ]


def parse_reply(frame: bytes) -> Reply:
    """Decode one already-CRC-checked-off-the-wire frame. Raises ValueError
    on anything that isn't a well-formed Modbus RTU reply — a real master
    would treat that the same way (retry, then give up), but for a test
    harness surfacing the exact shape mismatch is more useful than retrying."""
    if len(frame) < 5:
        raise ValueError(f"reply too short to be a Modbus RTU frame: {frame.hex()}")
    if crc16(frame) != 0:  # CRC over frame+checksum is 0 when valid
        raise ValueError(f"reply fails CRC check: {frame.hex()}")

    slave, function = frame[0], frame[1]
    if function & 0x80:
        if len(frame) != 5:
            raise ValueError(f"exception frame has the wrong length: {frame.hex()}")
        return Reply(slave, function & 0x7F, is_exception=True, exception_code=frame[2])

    byte_count = frame[2]
    data = frame[3 : 3 + byte_count]
    if len(data) != byte_count or len(frame) != 3 + byte_count + 2:
        raise ValueError(f"reply byte count doesn't match frame length: {frame.hex()}")
    return Reply(slave, function, is_exception=False, data=data)


class ModbusMaster:
    """Sends one Modbus-RTU request at a time and waits for a reply.

    Unlike ModbusSlave above, a master's request/reply cycle is inherently
    synchronous — there is no need for a background server thread, just
    "write a request, then read until the bus goes idle or we time out".
    """

    def __init__(
        self,
        port: str,
        baud: int = 9600,
        parity: int = 0,
        stop_bits: int = 1,
        timeout_s: float = 1.0,
    ):
        self.timeout_s = timeout_s
        self._serial = serial.Serial(
            port,
            baudrate=baud,
            parity=PARITY_MAP[parity],
            stopbits=stop_bits,
            timeout=0.05,
        )
        self._serial.reset_input_buffer()

    def close(self) -> None:
        self._serial.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def query(
        self, slave: int, function: int, reg_start: int, reg_count: int
    ) -> Reply | None:
        """Send one FC03/FC04 request and wait up to timeout_s for a reply.

        Returns None if nothing comes back within the window — the correct,
        distinguishable outcome for "address wasn't ours", which
        modbus_slave.c stays silent for by design (matching how a real slave
        behaves, see CLAUDE.md §13). Raises ValueError (via parse_reply) if
        something came back but wasn't a well-formed frame — that is a real
        difference from silence and callers should not conflate the two.
        """
        self._serial.reset_input_buffer()
        self._serial.write(build_request(slave, function, reg_start, reg_count))
        self._serial.flush()

        frame = self._read_frame()
        return None if frame is None else parse_reply(frame)

    def _read_frame(self) -> bytes | None:
        """Collect bytes until the bus falls idle, bounded by timeout_s
        overall. modbus_slave.c's own replies are short (5, or 3+N+2 bytes)
        so one idle-gap read is enough — unlike ModbusSlave._serve(), this
        does not need byte-at-a-time CRC resync: it is one request, one
        reply, then done, not an open-ended listener that has to survive
        indefinitely on a shared bus.
        """
        deadline = time.monotonic() + self.timeout_s
        buf = bytearray()
        last_byte = None
        while time.monotonic() < deadline:
            chunk = self._serial.read(64)
            now = time.monotonic()
            if chunk:
                buf += chunk
                last_byte = now
            elif buf and last_byte is not None and now - last_byte > 0.05:
                break
        return bytes(buf) if buf else None
