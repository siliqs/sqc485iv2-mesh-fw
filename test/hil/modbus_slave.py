"""
modbus_slave.py — a minimal Modbus-RTU slave driven from a USB-RS485 dongle.

Deliberately hand-rolled rather than pulled from pymodbus: the whole point of
this half of the HIL rig is to be an *independent* implementation of the
protocol. If both ends of the test came from the same library, a shared
misreading of the spec would pass.

Supports what the SQC485Iv2 poll engine actually issues: FC03 (read holding
registers) and FC04 (read input registers).
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field

import serial

PARITY_MAP = {0: serial.PARITY_NONE, 1: serial.PARITY_EVEN, 2: serial.PARITY_ODD}


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def registers_to_bytes(
    registers: dict[int, int], reg_start: int, reg_count: int
) -> bytes:
    """Return the data bytes a slave holding `registers` would serve for a read."""
    out = bytearray()
    for i in range(reg_count):
        out += registers.get(reg_start + i, 0).to_bytes(2, "big")
    return bytes(out)


def build_reply(
    frame: bytes,
    registers: dict[int, int],
    slave_id: int = 1,
    exception_code: int | None = None,
) -> bytes | None:
    """Build the RTU reply a slave would send for `frame`, or None to stay silent.

    Pure: no serial, no timing. The wired slave below and the mesh-side fake peer
    both go through here, so "what a slave would answer" has one definition.
    """
    if len(frame) < 8:
        return None
    slave, function = frame[0], frame[1]
    if slave != slave_id or function not in (3, 4):
        return None

    if exception_code is not None:
        # [addr][func|0x80][code] — five bytes with the checksum, whatever was
        # asked for. That length difference is what a master has to notice
        # before it can report the refusal instead of timing out.
        body = bytes([slave, function | 0x80, exception_code])
    else:
        reg_start = int.from_bytes(frame[2:4], "big")
        reg_count = int.from_bytes(frame[4:6], "big")
        data = registers_to_bytes(registers, reg_start, reg_count)
        body = bytes([slave, function, len(data)]) + data
    return body + crc16(body).to_bytes(2, "little")


@dataclass
class Request:
    slave: int
    function: int
    reg_start: int
    reg_count: int
    answered: bool
    at: float


@dataclass
class ModbusSlave:
    """A register file on a serial port, served in a background thread."""

    port: str
    slave_id: int = 1
    baud: int = 9600
    parity: int = 0
    stop_bits: int = 1
    registers: dict[int, int] = field(default_factory=dict)

    # Fault injection — lets the HIL prove the device's error paths end to end.
    drop_requests: bool = False
    # Answer every request with this Modbus exception code instead of data.
    # 0x02 (illegal data address) is what a real slave says when the poll plan
    # names a register it does not have.
    exception_code: int | None = None
    # Answer with a broken checksum. A noisy bus produces exactly this, and the
    # master must reject it rather than forward the bytes as a reading — nothing
    # downstream can tell a corrupted value from a real one.
    corrupt_crc: bool = False

    _serial: serial.Serial | None = None
    _thread: threading.Thread | None = None
    _stop: threading.Event = field(default_factory=threading.Event)
    _log: list[Request] = field(default_factory=list)
    _lock: threading.Lock = field(default_factory=threading.Lock)

    # ── lifecycle ────────────────────────────────────────────────────────────

    def start(self) -> None:
        self._serial = serial.Serial(
            self.port,
            baudrate=self.baud,
            parity=PARITY_MAP[self.parity],
            stopbits=self.stop_bits,
            timeout=0.05,
        )
        self._serial.reset_input_buffer()
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._serve, name="modbus-slave", daemon=True
        )
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.0)
        if self._serial:
            self._serial.close()
            self._serial = None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()

    # ── observation ──────────────────────────────────────────────────────────

    @property
    def requests(self) -> list[Request]:
        with self._lock:
            return list(self._log)

    def clear_log(self) -> None:
        with self._lock:
            self._log.clear()

    def value(self, addr: int) -> int:
        return self.registers.get(addr, 0)

    def expected_data(self, reg_start: int, reg_count: int) -> bytes:
        """Return the data bytes this slave would serve — what the device must forward."""
        return registers_to_bytes(self.registers, reg_start, reg_count)

    # ── the server loop ──────────────────────────────────────────────────────

    def _serve(self) -> None:
        buf = bytearray()
        last_byte = time.monotonic()

        while not self._stop.is_set():
            chunk = self._serial.read(64)
            now = time.monotonic()

            if chunk:
                buf += chunk
                last_byte = now
            elif buf and now - last_byte > 0.2:
                buf.clear()  # idle gap: whatever is left was never a whole frame

            # An FC03/FC04 request is exactly 8 bytes. Resync a byte at a time so a
            # partial frame or the tail of our own echo cannot wedge the parser.
            while len(buf) >= 8:
                frame = bytes(buf[:8])
                if crc16(frame) == 0:  # CRC over frame+checksum is 0 when valid
                    del buf[:8]
                    self._handle(frame)
                else:
                    del buf[:1]

    def _handle(self, frame: bytes) -> None:
        slave, function = frame[0], frame[1]
        reg_start = int.from_bytes(frame[2:4], "big")
        reg_count = int.from_bytes(frame[4:6], "big")

        serve = slave == self.slave_id and function in (3, 4) and not self.drop_requests

        with self._lock:
            self._log.append(
                Request(slave, function, reg_start, reg_count, serve, time.monotonic())
            )

        if not serve:
            return

        reply = build_reply(frame, self.registers, self.slave_id, self.exception_code)
        if reply is None:
            return
        if self.corrupt_crc:
            reply = reply[:-1] + bytes([reply[-1] ^ 0xFF])
        self._serial.write(reply)
        self._serial.flush()
        # A half-duplex converter reflects our own transmission; drop it so the
        # next request is not parsed out of our own echo.
        time.sleep(0.01)
        self._serial.reset_input_buffer()
