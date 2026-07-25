"""
serial_echo.py — turn the dongle into a dumb echo device.

The simplest possible bus test, and the one worth running before any Modbus
semantics: send a byte pattern, get the same pattern back. It has no addressing,
no function codes and no CRC, so when it fails there are only three things left
to suspect — the wiring, the line parameters, or the direction control.

It also splits the failure in half, which Modbus cannot:

    the dongle heard the pattern  →  the device's TX path and DE work
    the device got it back        →  the device's RX path and #RE work

On a two-wire bus the echo device hears its own transmission, so it holds off
reading for a moment after writing. Without that it would echo its own echo,
forever.
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field

import serial

PARITY_MAP = {0: serial.PARITY_NONE, 1: serial.PARITY_EVEN, 2: serial.PARITY_ODD}


@dataclass
class SerialEcho:
    port: str
    baud: int = 9600
    parity: int = 0
    stop_bits: int = 1

    _serial: serial.Serial | None = None
    _thread: threading.Thread | None = None
    _stop: threading.Event = field(default_factory=threading.Event)
    _heard: bytearray = field(default_factory=bytearray)
    _lock: threading.Lock = field(default_factory=threading.Lock)

    def start(self) -> None:
        self._serial = serial.Serial(
            self.port,
            baudrate=self.baud,
            parity=PARITY_MAP[self.parity],
            stopbits=self.stop_bits,
            timeout=0.02,
        )
        self._serial.reset_input_buffer()
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._serve, name="serial-echo", daemon=True
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

    @property
    def heard(self) -> bytes:
        """Everything received since the last clear() — what physically arrived."""
        with self._lock:
            return bytes(self._heard)

    def clear(self) -> None:
        with self._lock:
            self._heard.clear()

    def _serve(self) -> None:
        while not self._stop.is_set():
            chunk = self._serial.read(64)
            if not chunk:
                continue
            with self._lock:
                self._heard.extend(chunk)
            self._serial.write(chunk)
            self._serial.flush()
            # Our own transmission comes back at us on a two-wire bus. Drop it,
            # or the next read echoes the echo.
            time.sleep(len(chunk) * 10.0 / self.baud + 0.01)
            self._serial.reset_input_buffer()
