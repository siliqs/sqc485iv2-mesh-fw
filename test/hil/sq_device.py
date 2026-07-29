"""
sq_device.py — talking to an SQC485Iv2 over its USB console.

SQ commands travel as ordinary mesh packets on the private portnum, injected
through the local node's Meshtastic API. The firmware sees them with from==self
and answers straight back to the attached client without touching the radio
(ModbusModule::sendSqReply), so everything here is a local round trip.
"""

from __future__ import annotations

import os
import queue
import sys
import time

import meshtastic
import meshtastic.serial_interface
import serial
import sq_protocol as sq
from pubsub import pub


class DeviceError(RuntimeError):
    pass


class Device:
    def __init__(self, port: str, connect_timeout: float = 20.0):
        self.port = port
        self.connect_timeout = connect_timeout
        self.iface = None
        self.node_num = None
        self._rx: queue.Queue = queue.Queue()
        # Callbacks that see every packet on the private portnum, sender included.
        # The request/reply queue above is enough to DRIVE a node; answering as
        # one (see fake_peer.py) needs to know who asked.
        self._listeners: list = []

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
                # The library waits 300 s for the handshake by default. On a bench
                # that is indistinguishable from a hang — fail fast and retry
                # instead, since a cold CDC-ACM port usually needs a second go.
                self.iface = meshtastic.serial_interface.SerialInterface(
                    devPath=self.port, timeout=int(self.connect_timeout)
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

    def reboot(self, timeout: float = 120.0) -> float:
        """Reset the node and reconnect. Returns seconds until it answered again.

        Used by the checks that can only be made across a boot boundary — that
        the config really reached flash, and that this board (no bulk decoupling
        on 3.3V) comes back at all.

        The proof that a reset happened is `reboot_count`, NOT the /dev node.
        The obvious signal — waiting for the USB CDC to disappear and return —
        does not work here: the C3's USB-JTAG bridge re-enumerates so fast that
        the port node is never observably absent, even polling every 30 ms. A
        run built on that signal spends its whole timeout waiting for a
        disappearance that never comes, and then reports success for a board it
        never actually reset. myInfo.reboot_count is incremented by the firmware
        on every boot, so a value that has moved is positive evidence.
        """
        started = time.monotonic()
        before = getattr(self.iface.myInfo, "reboot_count", None)
        try:
            self.iface.localNode.reboot(secs=1)
        except Exception as exc:  # noqa: BLE001 — the reply may be cut off by the reset
            last = exc
        else:
            last = None
        self._teardown()
        time.sleep(2.0)  # let it actually go down before we start knocking

        deadline = started + timeout
        while time.monotonic() < deadline:
            try:
                self.connect(attempts=1)
            except DeviceError:
                time.sleep(0.5)
                continue
            after = getattr(self.iface.myInfo, "reboot_count", None)
            if before is None or after is None or after != before:
                return time.monotonic() - started
            # Answered, but on the same boot — the reset has not landed yet.
            self._teardown()
            time.sleep(0.5)

        raise DeviceError(
            f"the node did not reboot within {timeout:.0f}s "
            f"(reboot_count stayed at {before})"
            + (f"; reboot request errored: {last}" if last else "")
        )

    # ── packet plumbing ──────────────────────────────────────────────────────

    def add_listener(self, fn) -> None:
        """Call fn(packet) for every packet on the private portnum."""
        self._listeners.append(fn)

    def _on_receive(self, packet=None, interface=None):  # pubsub callback
        if not packet:
            return
        decoded = packet.get("decoded") or {}
        if decoded.get("portnum") not in (sq.PORTNUM, "PRIVATE_APP", str(sq.PORTNUM)):
            return
        self._rx.put((time.monotonic(), decoded.get("payload", b"")))
        for fn in self._listeners:
            try:
                fn(packet)
            except (
                Exception
            ) as exc:  # noqa: BLE001 — a bad listener must not stop the bus
                print(f"listener error: {exc}", file=sys.stderr)

    def drain(self) -> None:
        while True:
            try:
                self._rx.get_nowait()
            except queue.Empty:
                return

    def send(self, payload: bytes, destination=None) -> None:
        """Inject a packet on the private portnum; defaults to ourselves."""
        self.iface.sendData(
            payload,
            destinationId=self.node_num if destination is None else destination,
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

    def get_config(self, timeout: float = 6.0) -> tuple[sq.Config, bytes]:
        payload, _ = self.request(sq.CMD_GET_CONFIG, sq.RPL_GET_CONFIG, timeout)
        blob = payload[3:]
        return sq.parse_blob(blob), blob

    def apply_config(
        self, blob: bytes, timeout: float | None = None, attempts: int = 3
    ) -> int:
        """Push a config blob. Returns the status byte from the SQ! acknowledgement.

        A node polling a dead RS485 bus is blocked inside its poll loop and
        cannot answer until the cycle ends, so a fixed timeout is the wrong
        model: the wait has to be at least as long as the plan it is currently
        running can take. `blocked_for()` derives that, and the request is
        retried, because landing in a gap is partly luck.
        """
        if timeout is None:
            timeout = max(8.0, self.worst_case_block_s() + 4.0)
        last = None
        for attempt in range(attempts):
            try:
                payload, _ = self.request(blob, sq.RPL_CONFIG_ACK, timeout)
                return payload[3]
            except DeviceError as exc:
                last = exc
                if attempt + 1 < attempts:
                    time.sleep(2.0)
        raise DeviceError(f"config not acknowledged after {attempts} attempts: {last}")

    def worst_case_block_s(self) -> float:
        """How long the node's CURRENT plan can hold it inside its poll loop.

        Every poll costs (retries + 1) attempts, and on a silent bus each attempt
        costs a full response timeout. Firmware before the staged read charges two
        of those per attempt, so this assumes the worse of the two.
        """
        try:
            cfg, _ = self.get_config(timeout=6.0)
        except DeviceError:
            return 30.0  # cannot ask — assume something slow rather than give up early
        if not cfg.rs485_enabled or not cfg.polls:
            return 0.0
        per_attempt = 2 * cfg.response_timeout_ms / 1000.0
        return (
            len(cfg.polls) * (cfg.retries + 1) * per_attempt
            + len(cfg.polls) * cfg.poll_gap_ms / 1000.0
        )

    def quiesce(self) -> sq.Config:
        """Stop the node polling, so it can answer promptly.

        Reconfiguring a node that is mid-cycle against an unresponsive bus is a
        fight; taking RS485 out of the picture first turns every later exchange
        into a fast one. Returns the config that was in place beforehand.
        """
        # A node part-way through a plan against a dead bus can be blocked for
        # most of a cycle, so the first ask is expected to miss sometimes. Grow
        # the window instead of failing: this runs before nearly every check, and
        # a spurious raise here would abort a section that has not started yet.
        before = None
        for timeout in (6.0, 12.0, 25.0):
            try:
                before, _ = self.get_config(timeout=timeout)
                break
            except DeviceError as exc:
                last = exc
        if before is None:
            raise DeviceError(f"node would not answer SQG? while quiescing: {last}")
        idle = sq.Config(
            name=before.name,
            polls=[],
            rs485_enabled=False,
            uplink_interval_s=3600,
            deep_sleep=False,
        )
        self.apply_config(idle.to_blob())
        return before

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
    def channels(self):
        return self.iface.localNode.channels or []

    @property
    def firmware_version(self) -> str:
        return getattr(self.iface.metadata, "firmware_version", "?")
