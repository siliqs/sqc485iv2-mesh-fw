"""
sq_device.py — talking to an SQC485Iv2 over its USB console.

SQ commands travel as ordinary mesh packets on the private portnum, injected
through the local node's Meshtastic API. The firmware sees them with from==self
and answers straight back to the attached client without touching the radio
(ModbusModule::sendSqReply), so everything here is a local round trip.
"""

from __future__ import annotations

import queue
import time

import meshtastic
import meshtastic.serial_interface
import serial
import sq_protocol as sq
from pubsub import pub


class DeviceError(RuntimeError):
    pass


class Device:
    def __init__(self, port: str, connect_timeout: float = 30.0):
        self.port = port
        self.connect_timeout = connect_timeout
        self.iface = None
        self.node_num = None
        self._rx: queue.Queue = queue.Queue()

    # ── connection ───────────────────────────────────────────────────────────

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *exc):
        self.close()

    def connect(self, attempts: int = 5) -> None:
        """Open the interface, working around the cold-handshake failure.

        A CDC-ACM port hands over whatever the device wrote before we opened it.
        The library reads that stale tail as a malformed frame and the first
        handshake fails, so the port is drained raw before the library sees it.
        """
        last = None
        for _attempt in range(attempts):
            try:
                with serial.Serial(self.port, 115200, timeout=0.5) as raw:
                    raw.reset_input_buffer()
                    raw.reset_output_buffer()
                time.sleep(0.3)

                pub.subscribe(self._on_receive, "meshtastic.receive")
                self.iface = meshtastic.serial_interface.SerialInterface(
                    devPath=self.port
                )
                self.iface.waitForConfig()
                self.node_num = self.iface.getMyNodeInfo()["num"]
                return
            except (
                Exception
            ) as exc:  # noqa: BLE001 — retry on anything, report the last
                last = exc
                self._teardown()
                time.sleep(1.0)
        raise DeviceError(
            f"could not connect to {self.port} after {attempts} attempts: {last}"
        )

    def _teardown(self) -> None:
        try:
            pub.unsubscribe(self._on_receive, "meshtastic.receive")
        except Exception:  # noqa: BLE001
            pass
        if self.iface:
            try:
                self.iface.close()
            except Exception:  # noqa: BLE001
                pass
        self.iface = None

    def close(self) -> None:
        self._teardown()

    # ── packet plumbing ──────────────────────────────────────────────────────

    def _on_receive(self, packet=None, interface=None):  # pubsub callback
        if not packet:
            return
        decoded = packet.get("decoded") or {}
        if decoded.get("portnum") in (sq.PORTNUM, "PRIVATE_APP", str(sq.PORTNUM)):
            self._rx.put((time.monotonic(), decoded.get("payload", b"")))

    def drain(self) -> None:
        while True:
            try:
                self._rx.get_nowait()
            except queue.Empty:
                return

    def send(self, payload: bytes) -> None:
        """Inject a packet on the private portnum, addressed to ourselves."""
        self.iface.sendData(
            payload,
            destinationId=self.node_num,
            portNum=sq.PORTNUM,
            wantAck=False,
            wantResponse=False,
        )

    def await_reply(self, marker: bytes, timeout: float = 6.0) -> tuple[bytes, float]:
        """Wait for the next packet starting with ``marker``. Returns (payload, seconds)."""
        started = time.monotonic()
        deadline = started + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise DeviceError(
                    f"timed out after {timeout:.1f}s waiting for {marker!r}"
                )
            try:
                at, payload = self._rx.get(timeout=remaining)
            except queue.Empty:
                continue
            if payload.startswith(marker):
                return payload, at - started

    def request(
        self, command: bytes, marker: bytes, timeout: float = 6.0
    ) -> tuple[bytes, float]:
        self.drain()
        self.send(command)
        return self.await_reply(marker, timeout)

    # ── the SQ command set ───────────────────────────────────────────────────

    def capability(self) -> sq.Capability:
        payload, _ = self.request(sq.CMD_CAPABILITY, sq.RPL_CAPABILITY)
        return sq.parse_capability(payload)

    def get_config(self) -> tuple[sq.Config, bytes]:
        payload, _ = self.request(sq.CMD_GET_CONFIG, sq.RPL_GET_CONFIG)
        blob = payload[3:]
        return sq.parse_blob(blob), blob

    def apply_config(self, blob: bytes, timeout: float = 8.0) -> int:
        """Push a config blob. Returns the status byte from the SQ! acknowledgement."""
        payload, _ = self.request(blob, sq.RPL_CONFIG_ACK, timeout)
        return payload[3]

    def poll_now(self, timeout: float = 15.0) -> tuple[bytes, float]:
        """Trigger an immediate RS485 read; returns (raw-forward payload, latency)."""
        self.drain()
        self.send(sq.CMD_POLL_NOW)
        # The reply is a normal telemetry broadcast cc'd to us, so it is the next
        # packet that is NOT one of our own command markers.
        started = time.monotonic()
        deadline = started + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise DeviceError(f"no telemetry within {timeout:.1f}s of poll-now")
            try:
                at, payload = self._rx.get(timeout=remaining)
            except queue.Empty:
                continue
            # Telemetry starts with [slave][function]; a slave address of 'S' (0x53)
            # paired with function 'Q' (0x51) is not a legal Modbus reply, so the
            # magic can never collide with a real reading.
            if payload[:2] == b"SQ":
                continue  # one of our own command/reply markers
            return payload, at - started

    def rs485_bridge(
        self,
        frame: bytes,
        baud: int = 9600,
        parity: int = 0,
        stop_bits: int = 1,
        timeout: float = 6.0,
    ) -> bytes:
        """Put raw bytes on the RS485 line and return whatever the slave answered.

        The link parameters travel with the request (see sq.bridge_request) — the
        bridge is deliberately independent of the polling config, and omitting them
        does not fall back to it, it corrupts the frame.
        """
        payload, _ = self.request(
            sq.bridge_request(frame, baud, parity, stop_bits),
            sq.RPL_RS485_BRIDGE,
            timeout,
        )
        return payload[3:]

    # ── running configuration (Meshtastic side) ──────────────────────────────

    @property
    def lora(self):
        return self.iface.localNode.localConfig.lora

    @property
    def bluetooth(self):
        return self.iface.localNode.localConfig.bluetooth

    @property
    def firmware_version(self) -> str:
        return getattr(self.iface.metadata, "firmware_version", "?")
