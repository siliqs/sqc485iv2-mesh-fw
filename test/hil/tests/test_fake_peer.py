"""Tests for the software far end (fake_peer.py).

The peer is host software, so it is tested on the host: a stub device records
what it would have transmitted. No node, no radio, no RS485 — the point of the
fake peer is that the far side of a Siliqs link is defined entirely by what
appears on the mesh, and that is all this exercises.
"""

import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import sq_protocol as sq  # noqa: E402
from fake_peer import FakePeer  # noqa: E402
from modbus_slave import crc16  # noqa: E402

MASTER = 0x11223344


class StubDevice:
    """Stands in for Device: collects listeners, records what was sent."""

    def __init__(self):
        self.listeners = []
        self.sent = []
        self.node_num = 0x7D51BDB0

    def add_listener(self, fn):
        self.listeners.append(fn)

    def send(self, payload, destination=None):
        self.sent.append((payload, destination))

    def deliver(self, payload, frm=MASTER):
        """Feed the peer a packet as the node's API would."""
        packet = {"from": frm, "decoded": {"portnum": sq.PORTNUM, "payload": payload}}
        for fn in self.listeners:
            fn(packet)


def modbus_request(slave=1, function=3, reg_start=0, reg_count=2):
    frame = (
        bytes([slave, function])
        + reg_start.to_bytes(2, "big")
        + reg_count.to_bytes(2, "big")
    )
    return frame + crc16(frame).to_bytes(2, "little")


def tunnel_packet(frame, baud=9600):
    """Build what a tunnel master puts on the mesh: SQ} + link header + frame."""
    return b"SQ}" + sq.bridge_request(frame, baud)[3:]


def bridge_packet(frame, baud=9600):
    """Build what the USB tool puts on the mesh: SQ> + link header + frame."""
    return sq.bridge_request(frame, baud)


@pytest.fixture
def rig():
    dev = StubDevice()
    peer = FakePeer(dev, registers={0: 0xAA55, 1: 0x1234})
    return dev, peer


# ── answering ────────────────────────────────────────────────────────────────


def test_tunnel_request_is_answered_with_the_tunnel_marker(rig):
    dev, peer = rig
    dev.deliver(tunnel_packet(modbus_request()))

    assert len(dev.sent) == 1
    payload, destination = dev.sent[0]
    assert payload[:3] == b"SQ{"  # NOT SQ< — that is the USB tool's marker
    assert destination == MASTER  # answered to whoever asked
    assert payload[3:] == bytes([0x01, 0x03, 0x04, 0xAA, 0x55, 0x12, 0x34]) + crc16(
        bytes([0x01, 0x03, 0x04, 0xAA, 0x55, 0x12, 0x34])
    ).to_bytes(2, "little")
    assert peer.seen("tunnel") == 1


def test_bridge_request_is_answered_with_the_bridge_marker(rig):
    """The two callers must never cross: a USB probe of a peer must not look
    like a tunnel reply, or it gets injected onto that peer's bus."""
    dev, peer = rig
    dev.deliver(bridge_packet(modbus_request()))

    payload, _ = dev.sent[0]
    assert payload[:3] == b"SQ<"
    assert peer.seen("bridge") == 1
    assert peer.seen("tunnel") == 0


def test_echo_mode_returns_the_frame_untouched():
    dev = StubDevice()
    FakePeer(dev, mode="echo")
    frame = bytes(range(0x41, 0x49))
    dev.deliver(tunnel_packet(frame))

    payload, _ = dev.sent[0]
    assert payload == b"SQ{" + frame


def test_pipe_push_is_recorded_but_not_answered():
    """SQ~ is a one-way push; answering it would be a packet loop."""
    dev = StubDevice()
    peer = FakePeer(dev)
    dev.deliver(b"SQ~" + b"hello")

    assert dev.sent == []
    assert peer.seen("pipe") == 1
    assert peer.exchanges[0].request == b"hello"


def test_reads_for_another_slave_go_unanswered(rig):
    dev, peer = rig
    dev.deliver(tunnel_packet(modbus_request(slave=9)))

    assert dev.sent == []  # a real slave would also stay quiet
    assert peer.exchanges[0].reply is None


@pytest.mark.parametrize("payload", [b"", b"SQ", b"XY}", b"\x01\x03\xaa\x55"])
def test_traffic_that_is_not_ours_is_ignored(payload):
    dev = StubDevice()
    peer = FakePeer(dev)
    dev.deliver(payload)

    assert dev.sent == []
    assert peer.exchanges == []


def test_our_own_replies_are_not_answered_again():
    """The peer must not treat SQ{ / SQ< as requests, or two peers would loop."""
    dev = StubDevice()
    peer = FakePeer(dev)
    dev.deliver(b"SQ{" + modbus_request())
    dev.deliver(b"SQ<" + modbus_request())

    assert dev.sent == []
    assert peer.exchanges == []


# ── failure injection: what a real peer will not do on demand ────────────────


def test_exception_injection(rig):
    dev, _ = rig
    _, peer = rig
    peer.exception_code = 0x02
    dev.deliver(tunnel_packet(modbus_request()))

    payload, _ = dev.sent[0]
    body = payload[3:]
    assert body[1] == 0x83  # function | 0x80
    assert body[2] == 0x02  # illegal data address
    assert len(body) == 5  # an exception frame, whatever was asked


def test_silence(rig):
    dev, peer = rig
    peer.silent = True
    dev.deliver(tunnel_packet(modbus_request()))

    assert dev.sent == []
    assert peer.seen("tunnel") == 1  # it was seen, just not answered


def test_truncation(rig):
    dev, peer = rig
    peer.truncate_to = 3
    dev.deliver(tunnel_packet(modbus_request()))

    payload, _ = dev.sent[0]
    assert len(payload) == 3 + 3


def test_corrupt_checksum(rig):
    dev, peer = rig
    peer.corrupt_crc = True
    dev.deliver(tunnel_packet(modbus_request()))

    payload, _ = dev.sent[0]
    body = payload[3:]
    assert crc16(body[:-2]).to_bytes(2, "little") != body[-2:]


def test_wrong_marker(rig):
    """Proves the master rejects a tunnel answer that arrives on the USB tool's
    marker — the collision the two markers exist to prevent."""
    dev, peer = rig
    peer.wrong_marker = True
    dev.deliver(tunnel_packet(modbus_request()))

    payload, _ = dev.sent[0]
    assert payload[:3] == b"SQ<"  # answered a tunnel request as the tool


def test_exchange_log_records_both_sides(rig):
    dev, peer = rig
    dev.deliver(tunnel_packet(modbus_request()))
    dev.deliver(bridge_packet(modbus_request()))

    kinds = [e.kind for e in peer.exchanges]
    assert kinds == ["tunnel", "bridge"]
    assert all(e.frm == MASTER for e in peer.exchanges)
    assert all(e.reply for e in peer.exchanges)

    peer.clear()
    assert peer.exchanges == [] and peer.seen("tunnel") == 0
