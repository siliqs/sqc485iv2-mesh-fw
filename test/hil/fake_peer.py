"""
fake_peer.py — play the far end of a Siliqs link, in software.

Several SQ features are two-node by design: the RS485↔RS485 tunnel forwards a
frame to a peer that runs it on ITS bus and answers, and the USB pipe pushes
bytes to a peer that writes them out ITS port. Testing the near side has always
meant building the far side too — a second product node, wired to a second
Modbus slave, kept in step with the firmware.

It does not have to be a node. The far side is defined entirely by what it puts
on the mesh, so a plain Meshtastic node plus this file is a complete, and far
more capable, stand-in: no RS485 hardware, no test-only firmware, and it can
produce failures a real peer never would — silence, truncation, corruption, a
reply on the wrong marker, a reply that arrives too late.

    from fake_peer import FakePeer
    with Device(port) as dev, FakePeer(dev, registers={0: 0xAA55}) as peer:
        ...                       # drive the DUT; the peer answers
        assert peer.seen("tunnel") == 1

Run it standalone to leave a peer sitting on the bench:

    python3 test/hil/fake_peer.py --port /dev/cu.usbmodemXXXX --registers 0=0xAA55
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import threading
import time
from collections import Counter
from dataclasses import dataclass, field

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import sq_protocol as sq  # noqa: E402
from modbus_slave import build_reply  # noqa: E402
from sq_device import Device  # noqa: E402

# Requests this peer answers, and the marker each answer must carry. The two
# bridge callers use different markers so a manual USB probe of a peer is never
# mistaken for a tunnel reply and injected onto that peer's bus.
REPLY_MARKER = {
    sq.CMD_RS485_BRIDGE: b"SQ<",  # the USB / configurator tool
    b"SQ}": b"SQ{",  # the autonomous tunnel master
}


@dataclass
class Exchange:
    """One request the peer saw, and what it did about it."""

    kind: str
    frm: int
    request: bytes
    reply: bytes | None
    at: float


@dataclass
class FakePeer:
    """A programmable far end, driven through a node's API."""

    device: Device
    registers: dict[int, int] = field(default_factory=dict)
    slave_id: int = 1

    # ── what to answer with ──────────────────────────────────────────────────
    # "modbus" — parse the frame and answer as `registers` would
    # "echo"   — hand the frame straight back, whatever it is
    mode: str = "modbus"

    # ── failure injection: things a real peer will not do on demand ──────────
    exception_code: int | None = None  # answer every read with this Modbus exception
    silent: bool = False  # never answer
    delay_s: float = 0.0  # answer, but late
    truncate_to: int | None = None  # answer with only the first N bytes
    corrupt_crc: bool = False  # answer with a broken checksum
    wrong_marker: bool = False  # answer a tunnel request as if it were a bridge one

    _log: list = field(default_factory=list)
    _counts: Counter = field(default_factory=Counter)
    _lock: threading.Lock = field(default_factory=threading.Lock)

    def __post_init__(self):
        self.device.add_listener(self._on_packet)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.silent = True  # stop answering; the Device owns the connection

    # ── observation ──────────────────────────────────────────────────────────

    @property
    def exchanges(self) -> list[Exchange]:
        with self._lock:
            return list(self._log)

    def seen(self, kind: str) -> int:
        with self._lock:
            return self._counts[kind]

    def clear(self) -> None:
        with self._lock:
            self._log.clear()
            self._counts.clear()

    # ── the responder ────────────────────────────────────────────────────────

    def _on_packet(self, packet):
        decoded = packet.get("decoded") or {}
        payload = decoded.get("payload") or b""
        frm = packet.get("from")
        if len(payload) < 3 or not payload.startswith(b"SQ"):
            return

        marker = payload[:3]
        if marker == b"SQ~":
            # USB↔USB pipe: one-way by design, so there is nothing to answer.
            self._record("pipe", frm, payload[3:], None)
            return

        reply_marker = REPLY_MARKER.get(marker)
        if reply_marker is None:
            return  # not a request we play the far end of

        kind = "tunnel" if marker == b"SQ}" else "bridge"

        # Both carry the 6-byte link header before the frame (sq.bridge_request).
        body = payload[3:]
        frame = (
            body[sq.BRIDGE_HEADER_LEN :] if len(body) >= sq.BRIDGE_HEADER_LEN else body
        )

        answer = self._answer_for(frame)
        if answer is None or self.silent:
            self._record(kind, frm, frame, None)
            return

        if self.truncate_to is not None:
            answer = answer[: self.truncate_to]
        if self.corrupt_crc and len(answer) >= 1:
            answer = answer[:-1] + bytes([answer[-1] ^ 0xFF])
        if self.wrong_marker:
            reply_marker = b"SQ<" if reply_marker == b"SQ{" else b"SQ{"

        if self.delay_s:
            time.sleep(self.delay_s)

        self.device.send(reply_marker + answer, destination=frm)
        self._record(kind, frm, frame, answer)

    def _answer_for(self, frame: bytes) -> bytes | None:
        if self.mode == "echo":
            return frame
        if self.mode == "modbus":
            return build_reply(
                frame, self.registers, self.slave_id, self.exception_code
            )
        raise ValueError(f"unknown peer mode {self.mode!r}")

    def _record(self, kind, frm, request, reply):
        with self._lock:
            self._counts[kind] += 1
            self._log.append(Exchange(kind, frm, request, reply, time.monotonic()))


def _parse_registers(specs: list[str]) -> dict[int, int]:
    out = {}
    for spec in specs or []:
        addr, _, value = spec.partition("=")
        out[int(addr, 0)] = int(value, 0)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--port", required=True, help="the instrument node's USB console")
    ap.add_argument("--mode", default="modbus", choices=["modbus", "echo"])
    ap.add_argument("--slave-id", type=int, default=1)
    ap.add_argument(
        "--registers", nargs="*", metavar="ADDR=VALUE", help="e.g. 0=0xAA55 1=0x1234"
    )
    ap.add_argument(
        "--exception",
        type=lambda s: int(s, 0),
        help="answer every read with this exception code",
    )
    ap.add_argument("--silent", action="store_true", help="never answer")
    args = ap.parse_args()

    with Device(args.port) as dev:
        peer = FakePeer(
            dev,
            registers=_parse_registers(args.registers),
            slave_id=args.slave_id,
            mode=args.mode,
            exception_code=args.exception,
            silent=args.silent,
        )
        print(
            f"fake peer on {args.port} as node 0x{dev.node_num:08x} — mode {args.mode}"
            f"{', silent' if args.silent else ''}"
            f"{f', exception 0x{args.exception:02X}' if args.exception else ''}"
        )
        print(
            "  answers SQ} with SQ{ (tunnel) and SQ> with SQ< (bridge). Ctrl-C to stop."
        )
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
        print(
            f"\nseen: tunnel={peer.seen('tunnel')} bridge={peer.seen('bridge')} pipe={peer.seen('pipe')}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
