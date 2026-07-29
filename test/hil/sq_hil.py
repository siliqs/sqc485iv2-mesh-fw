#!/usr/bin/env python3
"""
sq_hil.py — hardware-in-the-loop acceptance run for the SQC485Iv2.

Everything the host tests cannot reach: that the built image on a real board
comes up on the certified radio profile, answers the configurator handshake,
survives a config round trip, and actually reads a Modbus slave over RS485 and
forwards the bytes unchanged.

Rig:
    SQC485Iv2 USB console  ──  this machine
    USB-RS485 dongle       ──  this machine, wired to the device's A/B terminals

The device's configuration is read before the run and written back afterwards,
so a HIL run leaves the board as it found it.

    python3 test/hil/sq_hil.py
    python3 test/hil/sq_hil.py --device /dev/cu.usbmodem844201 --rs485 /dev/cu.wchusbserial84410
    python3 test/hil/sq_hil.py --skip-rs485        # console-side checks only
"""

from __future__ import annotations

import argparse
import glob
import json
import pathlib
import re
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import qc_checks  # noqa: E402
import report as qc_report  # noqa: E402
import requirements as req  # noqa: E402
import sq_protocol as sq  # noqa: E402
from modbus_slave import ModbusSlave, crc16  # noqa: E402
from serial_echo import SerialEcho  # noqa: E402
from sq_device import Device, DeviceError  # noqa: E402

REPO = HERE.parents[1]

GREEN, RED, YELLOW, DIM, BOLD, OFF = (
    "\033[32m",
    "\033[31m",
    "\033[33m",
    "\033[2m",
    "\033[1m",
    "\033[0m",
)

# ── certified radio profile ──────────────────────────────────────────────────
# The Taiwan DTS envelope the product is type-approved for. These are factory
# defaults a user is allowed to change, so a mismatch is reported as a warning
# unless --strict-radio is given.
CERTIFIED_LORA = {
    "use_preset": False,
    "bandwidth": 500,
    "spread_factor": 9,
    "coding_rate": 5,
    "override_frequency": 922.5,
    "tx_power": 22,
    "region": 8,  # RegionCode_TW
}


class Report:
    """Assertion output, plus which requirement each assertion was answering.

    The verdict a release gate owes you is per-requirement, not per-assertion: a
    run that prints forty green ticks still tells you nothing about the feature
    it never touched. Checks call `requirement(id)` before their assertions, and
    everything until the next call is attributed there; anything in
    requirements.py that nothing claimed comes out as NOT RUN.
    """

    def __init__(self):
        self.failures = 0
        self.warnings = 0
        self.timings: dict[str, float] = {}
        self.status: dict[str, str] = {r.id: req.NOT_RUN for r in req.REQUIREMENTS}
        self.notes: dict[str, str] = {}
        self._current: str | None = None

    # ── requirement attribution ──────────────────────────────────────────────

    def requirement(self, req_id: str):
        if req_id not in self.status:
            raise KeyError(f"{req_id} is not in requirements.py")
        self._current = req_id
        if self.status[req_id] == req.NOT_RUN:
            self.status[req_id] = req.PASS  # until something fails against it

    def skip(self, req_id: str, why: str):
        if self.status.get(req_id) in (req.NOT_RUN, None):
            self.status[req_id] = req.SKIP
            self.notes[req_id] = why

    def _mark(self, state):
        if self._current and not (
            state == req.PASS and self.status[self._current] == req.FAIL
        ):
            self.status[self._current] = state

    # ── output ───────────────────────────────────────────────────────────────

    def ok(self, msg):
        self._mark(req.PASS)
        print(f"  {GREEN}✓{OFF} {msg}")

    def bad(self, msg, detail=None):
        self.failures += 1
        self._mark(req.FAIL)
        if self._current:
            self.notes[self._current] = msg
        print(f"  {RED}✗{OFF} {msg}")
        if detail:
            print(f"    {DIM}{detail}{OFF}")

    def warn(self, msg, detail=None):
        self.warnings += 1
        print(f"  {YELLOW}!{OFF} {msg}")
        if detail:
            print(f"    {DIM}{detail}{OFF}")

    def info(self, msg):
        print(f"    {DIM}{msg}{OFF}")

    def check(self, cond, ok_msg, bad_msg, detail=None):
        self.ok(ok_msg) if cond else self.bad(bad_msg, detail)
        return cond

    def section(self, title):
        self._current = None
        print(f"\n{BOLD}{title}{OFF}")

    # ── the checklist ────────────────────────────────────────────────────────

    def checklist(self):
        """Print every requirement, in order, with its verdict."""
        mark = {
            req.PASS: f"{GREEN}PASS{OFF}",
            req.FAIL: f"{RED}FAIL{OFF}",
            req.SKIP: f"{YELLOW}SKIP{OFF}",
            req.NOT_RUN: f"{RED}NOT RUN{OFF}",
        }
        print(f"\n{BOLD}acceptance checklist{OFF}")
        area = None
        for requirement in req.REQUIREMENTS:
            if requirement.area != area:
                area = requirement.area
                print(f"  {DIM}── {area} {'─' * (58 - len(area))}{OFF}")
            state = self.status[requirement.id]
            print(f"  {mark[state]:<16} {requirement.id:<8} {requirement.title}")
            note = self.notes.get(requirement.id)
            if note and state != req.PASS:
                print(f"                   {DIM}{note}{OFF}")

    def gaps(self):
        return [r for r in req.REQUIREMENTS if self.status[r.id] == req.NOT_RUN]


def find_port(patterns, label, flag):
    """Resolve exactly one port, or say what to pick from.

    Guessing is worse than asking here: a Mac typically has several usbmodem
    nodes, and connecting to the wrong one fails several seconds later with a
    protocol timeout that looks like a firmware bug.
    """
    hits = sorted({hit for pattern in patterns for hit in glob.glob(pattern)})
    if not hits:
        raise SystemExit(
            f"no {label} found (looked for {', '.join(patterns)}) — pass {flag}"
        )
    if len(hits) > 1:
        listing = "\n  ".join(hits)
        raise SystemExit(
            f"several candidates for the {label}; pick one with {flag}:\n  {listing}"
        )
    return hits[0]


sys.path.insert(0, str(REPO / "test" / "e2e"))
from sqc485iv2_e2e import firmware_provenance  # noqa: E402


def source_fw_version() -> str | None:
    header = REPO / "src" / "siliqs" / "firmware_core" / "include" / "config.h"
    match = re.search(r'#define\s+SQ_FW_VERSION\s+"([^"]+)"', header.read_text())
    return match.group(1) if match else None


# ── checks ───────────────────────────────────────────────────────────────────


def check_identity(dev: Device, rep: Report):
    rep.section("identity")
    rep.info(f"node 0x{dev.node_num:08x}, firmware {dev.firmware_version}")

    cap = dev.capability()
    rep.info(
        f"capability: {cap.product_id} fw {cap.firmware}, blob v{cap.max_blob_version}, "
        f"proto {cap.proto}, features 0x{cap.features:02x} ({', '.join(cap.feature_list())})"
    )

    rep.requirement("ID-01")
    rep.check(
        cap.product_id == "SQC485Iv2",
        f"product id is {cap.product_id}",
        f"product id is {cap.product_id!r}, expected SQC485Iv2",
        "the configurator labels nodes from this string",
    )

    rep.requirement("ID-02")
    rep.check(
        cap.proto == 2,
        "capability proto 2",
        f"capability proto {cap.proto}, expected 2",
    )
    rep.requirement("ID-03")
    rep.check(
        cap.features == 0x3F,
        f"all six features advertised (0x{cap.features:02x})",
        f"features 0x{cap.features:02x}, expected 0x3F",
    )

    rep.requirement("ID-04")
    expected_fw = source_fw_version()
    if expected_fw:
        rep.check(
            cap.firmware == expected_fw,
            f"reports fw {cap.firmware}, matching the source tree",
            f"reports fw {cap.firmware}, source says {expected_fw}",
            "the board is running a different build than this checkout",
        )

    # SQ_FW_VERSION alone does not identify a build. It was "1.3.3" on both main
    # and this branch across a 613-line firmware difference, so the string agreed
    # while the images did not -- exactly the case a release gate exists to catch.
    # The git hash in firmware_version does identify it, and comparing the tree
    # objects of the firmware paths (rather than the commit) keeps a docs-only
    # commit from failing the gate for no reason.
    ok, why = firmware_provenance(REPO, dev.firmware_version)
    if ok is None:
        rep.info(f"provenance not checked: {why}")
    else:
        rep.check(ok, why, why, "SQ_FW_VERSION agreeing is not enough on its own")
    return cap


def check_radio(dev: Device, rep: Report, strict: bool):
    rep.section("radio profile (running config)")
    lora = dev.lora
    complain = rep.bad if strict else rep.warn

    rep.requirement("RF-01")
    for field, want in CERTIFIED_LORA.items():
        got = getattr(lora, field, None)
        match = abs(got - want) < 1e-6 if isinstance(want, float) else got == want
        if match:
            rep.ok(f"{field} = {got}")
        else:
            complain(
                f"{field} = {got}, certified value is {want}",
                "a user may legitimately have changed this; use --strict-radio "
                "to treat it as a failure (e.g. when verifying a factory image)",
            )

    rep.requirement("RF-02")
    if dev.bluetooth.enabled:
        rep.warn(
            "bluetooth is enabled",
            "the factory default for this product is off; expected on a board that "
            "has been configured by hand, not on a fresh factory image",
        )
    else:
        rep.ok("bluetooth disabled (factory default)")


# The expansion of the well-known "AQ==" default key. This is a belt-and-braces
# check only -- the length gates in check_factory_channel are what carry RF-03, so
# the requirement stays correct even if upstream ever changes this value.
PUBLIC_DEFAULT_PSK = bytes.fromhex("d4f1bb3a20290759f0bcffabcf4e6901")


def check_factory_channel(dev: Device, rep: Report):
    """RF-03 — the board is claimed to be a shipping image; prove it is provisioned.

    Reports lengths and verdicts, not the PSK itself. Not for secrecy — the factory
    key is a published value (see CLAUDE.md §8) — but because a hex blob in a report
    is noise no reader can act on, and the value belongs in exactly one place.

    Note this check is about provisioning, not privacy: it proves the image carries
    the factory channel rather than the stock Meshtastic default, which is what
    separates a shipping image from a CI artifact.
    """
    rep.section("factory channel provisioning")
    rep.requirement("RF-03")

    channels = dev.channels
    if len(channels) < 2:
        rep.bad(
            f"no channel[1]: the node has {len(channels)} channel(s)",
            "a shipping image is built with SQ_FACTORY_CH1_NAME/_PSK_HEX, which "
            "provisions the secondary channel; a CI artifact has neither",
        )
        return

    settings = channels[1].settings
    name = getattr(settings, "name", "") or ""
    psk = bytes(getattr(settings, "psk", b"") or b"")

    if name:
        rep.ok(f"channel[1] name = {name!r}")
    else:
        rep.bad(
            "channel[1] has no name",
            "SQ_FACTORY_CH1_NAME was empty or unset at build time",
        )

    if len(psk) in (16, 32):
        rep.ok(f"channel[1] psk is {len(psk)} bytes (AES{len(psk) * 8})")
    elif not psk:
        rep.bad(
            "channel[1] has no psk — the channel is unencrypted",
            "an unencrypted factory channel is readable and writable by anyone in range",
        )
    elif len(psk) == 1:
        rep.bad(
            "channel[1] psk is a 1-byte default key index",
            "a 1-byte psk selects a published Meshtastic default key; any stock app "
            "joins this node out of the box",
        )
    else:
        rep.bad(
            f"channel[1] psk is {len(psk)} bytes, not a valid AES key length",
            "Meshtastic accepts 0, 1, 16 or 32 only. Anything else was corrupted on "
            "its way into the build, and the node ships looking flashed while "
            "matching none of its peers",
        )

    if psk == PUBLIC_DEFAULT_PSK:
        rep.bad(
            "channel[1] psk is the published Meshtastic default key",
            "the same key every stock installation ships with, so the channel is "
            "public regardless of how long the key is",
        )


def check_config_roundtrip(dev: Device, rep: Report, saved_blob: bytes):
    rep.section("config round trip")

    probe = sq.Config(
        name="hil-probe",
        polls=[sq.Poll(7, 4, 100, 3), sq.Poll(2, 3, 40, 1)],
        baud=19200,
        parity=2,
        stop_bits=2,
        response_timeout_ms=750,
        retries=1,
        poll_gap_ms=45,
        uplink_interval_s=3600,
        deep_sleep=False,
        tx_dest=0x0A0B0C0D,
        tx_channel=2,
        confirmed=True,
    )

    dev.quiesce()  # a node mid-poll against a dead bus cannot answer promptly
    started = time.monotonic()
    status = dev.apply_config(probe.to_blob())
    rep.timings["config apply"] = time.monotonic() - started

    if not rep.check(
        status == 0,
        "config blob accepted",
        f"config rejected: {sq.CONFIG_STATUS.get(status, status)}",
    ):
        return

    rep.requirement("CFG-01")
    read_back, _ = dev.get_config()
    fields = [
        ("name", probe.name, read_back.name),
        ("baud", probe.baud, read_back.baud),
        ("parity", probe.parity, read_back.parity),
        ("stop_bits", probe.stop_bits, read_back.stop_bits),
        (
            "response_timeout_ms",
            probe.response_timeout_ms,
            read_back.response_timeout_ms,
        ),
        ("retries", probe.retries, read_back.retries),
        ("poll_gap_ms", probe.poll_gap_ms, read_back.poll_gap_ms),
        ("uplink_interval_s", probe.uplink_interval_s, read_back.uplink_interval_s),
        ("deep_sleep", probe.deep_sleep, read_back.deep_sleep),
        ("tx_dest", probe.tx_dest, read_back.tx_dest),
        ("tx_channel", probe.tx_channel, read_back.tx_channel),
        ("confirmed", probe.confirmed, read_back.confirmed),
        ("poll_count", len(probe.polls), len(read_back.polls)),
    ]
    mismatched = [(n, w, g) for n, w, g in fields if w != g]
    rep.check(
        not mismatched,
        "every field survived write → read-back",
        f"{len(mismatched)} field(s) did not survive the round trip",
        "; ".join(f"{n}: wrote {w!r}, read {g!r}" for n, w, g in mismatched),
    )

    rep.requirement("CFG-02")
    for i, (wrote, got) in enumerate(zip(probe.polls, read_back.polls, strict=False)):
        rep.check(
            wrote.as_tuple() == got.as_tuple(),
            f"poll[{i}] round-tripped: slave {got.slave} fc{got.function} "
            f"@{got.reg_start}×{got.reg_count}",
            f"poll[{i}] wrote {wrote.as_tuple()}, read {got.as_tuple()}",
        )

    # A plan the radio cannot carry must be refused, not accepted and truncated.
    # The widest the format can express is 8 polls x 16 registers = 272 bytes
    # against a 233-byte packet; the device drops whole polls off the end, which
    # is graceful but invisible, so it should never get that far.
    oversized = sq.Config(
        name="hil-oversize",
        polls=[sq.Poll(i + 1, 3, i * 16, 16) for i in range(8)],
        uplink_interval_s=3600,
        deep_sleep=False,
    )
    rep.requirement("CFG-03")
    rep.info(
        f"oversized plan: {len(oversized.polls)} polls need "
        f"{oversized.expected_payload_len} bytes, a packet carries {sq.MESH_PAYLOAD_LEN}"
    )
    status = dev.apply_config(oversized.to_blob())
    rep.check(
        status == 3,
        "an over-sized poll plan is refused (status 3)",
        f"an over-sized plan returned {sq.CONFIG_STATUS.get(status, status)} — "
        "the last polls would be dropped from every uplink with nothing to say so",
    )

    rep.check(
        dev.apply_config(saved_blob) == 0,
        "device configuration restored",
        "could not restore the original configuration — the board is left on the probe config",
    )


def diagnose_bus(dev: Device, rs485_port: str, baud: int, rep: Report):
    """Separate 'nothing is connected' from 'connected but mis-wired'.

    A silent bus and a polarity-swapped bus fail identically at the Modbus layer,
    but they look completely different on the wire — so make the device transmit
    and dump what the dongle physically hears, rather than guessing.
    """
    import threading

    import serial as pyserial

    rep.info("diagnosing the bus: transmitting while listening raw")

    heard = bytearray()
    stop = threading.Event()

    def listen():
        try:
            with pyserial.Serial(rs485_port, baud, timeout=0.2) as line:
                line.reset_input_buffer()
                while not stop.is_set():
                    heard.extend(line.read(256))
        except Exception as exc:  # noqa: BLE001
            rep.info(f"could not open {rs485_port} to listen: {exc}")

    listener = threading.Thread(target=listen, daemon=True)
    listener.start()
    time.sleep(0.4)

    frame = bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x02])
    frame += crc16(frame).to_bytes(2, "little")
    try:
        dev.rs485_bridge(frame, timeout=8.0)
    except DeviceError:
        pass
    time.sleep(0.5)
    stop.set()
    listener.join(timeout=2.0)

    if not heard:
        rep.info("the dongle heard NOTHING while the device transmitted")
        rep.info(
            "→ no electrical path: check A/B are actually landed, and that the "
            "device's RS485 transceiver has power"
        )
    elif set(heard) <= {0x00, 0xFF}:
        rep.info(
            f"the dongle heard {len(heard)} bytes, all {heard[:8].hex()} — "
            "a continuous break, not data"
        )
        rep.info(
            "→ the line is being driven but cannot be framed. The usual causes are "
            "A/B swapped, or no common ground between the dongle and the device"
        )
    elif heard.startswith(frame):
        rep.info(f"the dongle heard the request cleanly ({heard[:16].hex()})")
        rep.info(
            "→ wiring is fine; the slave simply did not answer (wrong slave id, or "
            "the dongle is not in listen mode)"
        )
    else:
        rep.info(f"the dongle heard {len(heard)} bytes: {heard[:32].hex()}")
        rep.info(
            f"→ garbled: the device sent {frame.hex()}. Check that both ends agree "
            f"on {baud} 8N1"
        )


def check_rs485_echo(
    dev: Device, rep: Report, rs485_port: str, baud: int = 9600
) -> bool:
    """Prove the bus works before asking anything of the Modbus engine.

    Runs first because a failure here has a much smaller suspect list, and because
    every Modbus symptom downstream would otherwise be explained twice.
    """
    rep.section("RS485 loopback")

    pattern = bytes(
        range(0x41, 0x49)
    )  # "ABCDEFGH" — printable, easy to spot on a scope

    rep.requirement("BUS-01")
    with SerialEcho(port=rs485_port, baud=baud) as echo:
        time.sleep(0.3)
        echo.clear()
        returned = dev.rs485_bridge(pattern, baud=baud)
        heard = echo.heard

    if heard == pattern:
        rep.ok(f"the dongle heard exactly what the device sent ({heard.hex()})")
    elif heard:
        rep.bad(
            f"the dongle heard {heard.hex()}, the device sent {pattern.hex()}",
            f"the line is carrying data but it is being mangled — check both ends are at "
            f"{baud} 8N1",
        )
        return False
    else:
        rep.bad(
            "the dongle heard nothing while the device transmitted",
            "the device's TX path or DE (GPIO9, polarity per board rev) is not driving the bus",
        )
        return False

    rep.requirement("BUS-02")
    if returned == pattern:
        rep.ok(f"the device read the echo back ({returned.hex()})")
    elif returned:
        rep.bad(
            f"the device read back {returned.hex()}, expected {pattern.hex()}",
            "TX works and the echo came back mangled — suspect the TX-echo strip or framing",
        )
        return False
    else:
        rep.bad(
            "the device transmitted but read nothing back",
            "TX works, RX does not: #RE (GPIO2, active low) must be held LOW, and DE must be "
            "released after the write",
        )
        return False

    return True


def check_rs485(dev: Device, rep: Report, rs485_port: str):
    rep.section("RS485 / Modbus (end to end)")

    plan = sq.Config(
        name="hil-modbus",
        polls=[sq.Poll(1, 3, 0, 2), sq.Poll(1, 3, 10, 1)],
        baud=9600,
        response_timeout_ms=1000,
        retries=1,
        poll_gap_ms=20,
        uplink_interval_s=3600,  # keep periodic uplinks out of the way
        deep_sleep=False,  # and keep the node awake for the whole run
    )
    dev.quiesce()
    if dev.apply_config(plan.to_blob()) != 0:
        rep.bad("could not install the RS485 test plan")
        return

    slave = ModbusSlave(
        port=rs485_port,
        slave_id=1,
        baud=plan.baud,
        registers={0: 0xAA55, 1: 0x1234, 10: 0xBEEF},
    )

    with slave:
        time.sleep(0.3)

        # ── the bus itself, before involving the poll engine ──────────────────
        request = bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x02])
        request += crc16(request).to_bytes(2, "little")
        expect = bytes([0x01, 0x03, 0x04]) + slave.expected_data(0, 2)
        try:
            answer = dev.rs485_bridge(request, baud=plan.baud)
        except DeviceError as exc:
            answer = b""
            rep.info(f"raw bridge did not reply: {exc}")

        if not answer:
            rep.bad("the slave never answered — nothing came back off the bus")
            diagnose_bus(dev, rs485_port, plan.baud, rep)
            rep.info("skipping the poll checks — fix the bus first")
            return

        rep.requirement("BUS-03")
        if not rep.check(
            answer.startswith(expect),
            f"raw bridge reached the slave ({answer.hex()})",
            f"raw bridge returned {answer.hex()}, expected a frame starting {expect.hex()}",
        ):
            diagnose_bus(dev, rs485_port, plan.baud, rep)
            rep.info("skipping the poll checks — fix the bus first")
            return

        # ── the poll engine ──────────────────────────────────────────────────
        slave.clear_log()
        payload, latency = dev.poll_now()
        rep.timings["poll-now round trip"] = latency

        rep.requirement("POLL-01")
        expected = (
            bytes([0x01, 0x03])
            + slave.expected_data(0, 2)
            + bytes([0x01, 0x03])
            + slave.expected_data(10, 1)
        )
        rep.check(
            payload == expected,
            f"raw-forward payload is byte-exact ({len(payload)} bytes, {latency*1000:.0f} ms)",
            f"payload {payload.hex()} != expected {expected.hex()}",
        )

        rep.requirement("POLL-02")
        rep.check(
            len(payload) == plan.expected_payload_len,
            f"payload length matches the plan ({len(payload)} bytes)",
            f"payload is {len(payload)} bytes, the plan predicts {plan.expected_payload_len}",
        )

        rep.requirement("POLL-03")
        served = [r for r in slave.requests if r.answered]
        rep.check(
            len(served) == len(plan.polls),
            f"slave saw exactly {len(plan.polls)} requests",
            f"slave saw {len(served)} requests for {len(plan.polls)} polls",
        )

        # Decode the way the cloud does — from the config alone.
        for reading in sq.split_raw_payload(payload, plan.polls):
            if reading.error:
                rep.bad(reading.describe())
            else:
                rep.ok(reading.describe())

        # ── a slave that refuses ─────────────────────────────────────────────
        # "Illegal data address" is what a real slave says when the poll plan
        # names a register it does not have. It must be reported as a refusal
        # rather than a timeout, and it must not burn the retry budget: the
        # answer will not change however many times it is asked.
        slave.exception_code = 0x02
        slave.clear_log()
        payload, latency = dev.poll_now(timeout=20.0)
        rep.timings["poll-now against a refusing slave"] = latency

        rep.requirement("POLL-07")
        decoded = sq.split_raw_payload(payload, plan.polls)
        rep.check(
            all(r.error == "exception" for r in decoded),
            "a refusing slave is reported as an exception, not a timeout",
            f"errors were {[r.error for r in decoded]}, expected 'exception' — "
            "a wrong register in the plan would look like an unplugged cable",
        )
        # And the payload has to say WHICH refusal, or the technician still cannot
        # tell "fix the poll plan" from "check the device".
        rep.requirement("POLL-08")
        rep.check(
            all(r.exception_code == 0x02 for r in decoded),
            f"the slave's own reason survives to the payload ({decoded[0].describe()})",
            f"exception codes were {[r.exception_code for r in decoded]}, expected 0x02",
        )
        rep.requirement("POLL-09")
        rep.check(
            len(slave.requests) == len(plan.polls),
            f"each poll asked exactly once ({len(slave.requests)} requests)",
            f"{len(slave.requests)} requests for {len(plan.polls)} polls — a permanent "
            "refusal should not be retried",
        )
        rep.check(
            latency < 1.0,
            f"refusal returned in {latency * 1000:.0f} ms, no retry budget spent",
            f"refusal took {latency * 1000:.0f} ms — retries are still being spent on it",
        )
        slave.exception_code = None

        # ── the error path ───────────────────────────────────────────────────
        # A dead slave must still produce a full-length frame, or every later
        # value in the cloud decode shifts.
        slave.drop_requests = True
        slave.clear_log()
        payload, latency = dev.poll_now(timeout=30.0)
        rep.timings["poll-now with dead bus"] = latency

        rep.requirement("POLL-06")
        rep.check(
            len(payload) == plan.expected_payload_len,
            f"dead bus still produced a full-length payload ({len(payload)} bytes)",
            f"dead bus produced {len(payload)} bytes, expected {plan.expected_payload_len}",
            "a short payload desynchronises the cloud decoder for every following poll",
        )

        decoded = sq.split_raw_payload(payload, plan.polls)
        rep.check(
            all(r.error for r in decoded),
            f"every poll reported an error frame ({decoded[0].error})",
            "a poll reported success against a slave that answered nothing",
        )


# ── entry point ──────────────────────────────────────────────────────────────


def _dut_identity(dev: Device) -> dict:
    """What the report needs to say which board and which build this was."""
    out = {
        "node": f"!{dev.node_num:08x}",
        "firmware": dev.firmware_version,
        "pio_env": getattr(dev.iface.myInfo, "pio_env", ""),
        "reboot_count": getattr(dev.iface.myInfo, "reboot_count", ""),
    }
    try:
        cap = dev.capability()
        out.update(
            product_id=cap.product_id,
            sq_fw=cap.firmware,
            blob_version=f"v{cap.max_blob_version}",
            features=f"0x{cap.features:02x} ({', '.join(cap.feature_list())})",
        )
    except DeviceError:
        pass
    return out


def _run(rep: Report, fn, *args, **kwargs):
    """Run one section; a crash inside it fails that section, not the run.

    A checklist stops being a checklist the moment one unhandled exception can
    end it early — everything below the crash silently becomes NOT RUN, which
    reads like nobody wrote those checks rather than like they were skipped.
    """
    try:
        return fn(*args, **kwargs)
    except Exception as exc:  # noqa: BLE001 — this is the isolation boundary
        rep.bad(
            f"{fn.__name__} did not complete: {type(exc).__name__}: {exc}",
            "the rest of the checklist still ran; this section's requirements are "
            "reported on whatever it managed to assert before the failure",
        )
        return None


def _skip_area(rep: Report, needs: str, why: str):
    """Mark every requirement gated on `needs` as skipped, with the reason.

    A skip has to be recorded, not silently omitted: SKIP means "this bench
    cannot answer it", NOT RUN means "nobody wrote a check". Collapsing the two
    is exactly how a gap stops being visible.
    """
    for requirement in req.REQUIREMENTS:
        if requirement.needs == needs:
            rep.skip(requirement.id, why)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--device", help="SQC485Iv2 USB console (default: autodetect)")
    ap.add_argument("--rs485", help="USB-RS485 dongle (default: autodetect)")
    ap.add_argument(
        "--peer",
        metavar="PORT",
        help="a second Meshtastic node, used as the far end of the RS485 tunnel "
        "(FEAT-02). Without it that requirement is skipped: the tunnel forward is "
        "sent with ccToPhone=false and is therefore invisible from the master",
    )
    ap.add_argument(
        "--skip-rs485", action="store_true", help="console-side checks only"
    )
    ap.add_argument(
        "--strict-radio",
        action="store_true",
        help="treat a radio profile deviation as a failure (factory image verification)",
    )
    ap.add_argument(
        "--factory",
        action="store_true",
        help="assert that the board is running a SHIPPING image, and verify it "
        "carries the factory channel with a private PSK (RF-03). Without it that "
        "requirement is skipped, because a CI artifact has no factory channel by "
        "construction and failing it there would be noise",
    )
    ap.add_argument(
        "--json", metavar="PATH", help="also write the timings to a JSON file"
    )
    ap.add_argument(
        "--skip-reboot",
        action="store_true",
        help="skip the checks that reset the board (persistence, boot reliability)",
    )
    ap.add_argument(
        "--deep-sleep",
        action="store_true",
        help="also verify the duty-cycle deep sleep (FEAT-03). Puts the node on "
        "CLIENT_MUTE for the duration and restores the role afterwards; slow, and "
        "the board only offers short windows while it is running",
    )
    ap.add_argument(
        "--boot-rounds",
        type=int,
        default=3,
        metavar="N",
        help="resets for the boot-reliability check (default 3)",
    )
    ap.add_argument(
        "--report-dir",
        metavar="DIR",
        default=str(HERE / "reports"),
        help="where to record this run (default: test/hil/reports). One dated "
        "Markdown file per run, plus a PDF rendering of it",
    )
    ap.add_argument(
        "--no-report",
        action="store_true",
        help="do not record this run to a file",
    )
    ap.add_argument(
        "--allow-gaps",
        action="store_true",
        help="do not fail the run just because some requirements were never "
        "exercised (use when deliberately running a subset)",
    )
    args = ap.parse_args()

    device_port = args.device or find_port(
        ["/dev/cu.usbmodem*"], "SQC485Iv2 console", "--device"
    )
    rs485_port = None
    if not args.skip_rs485:
        # A CH340 dongle enumerates under both the WCH and the generic namespace;
        # prefer the WCH node and never offer the same adapter twice.
        rs485_port = args.rs485 or find_port(
            (
                ["/dev/cu.wchusbserial*"]
                if glob.glob("/dev/cu.wchusbserial*")
                else ["/dev/cu.usbserial-*"]
            ),
            "USB-RS485 dongle",
            "--rs485",
        )

    print(f"\n{BOLD}SQC485Iv2 hardware in the loop{OFF}")
    print(f"  {DIM}console {device_port}{OFF}")
    print(f"  {DIM}rs485   {rs485_port or 'skipped'}{OFF}")

    rep = Report()
    started = time.monotonic()
    started_at, started_human = qc_report.now()
    meta = {
        "started": started_at,
        "started_human": started_human,
        "device_port": device_port,
        "rs485_port": rs485_port,
        "peer_port": args.peer,
        "command": " ".join(pathlib.Path(a).name if a == sys.argv[0] else a
                            for a in sys.argv),
        "git": qc_report.git_context(REPO),
        "dut": {},
    }

    with Device(device_port) as dev:
        saved_cfg, saved_blob = dev.get_config()
        # Capture identity now: the report has to survive whatever the run does
        # to the board afterwards, and `dev` is closed by the time it is written.
        meta["dut"] = _dut_identity(dev)
        print(
            f"  {DIM}saved device config: {saved_cfg.name!r}, {len(saved_cfg.polls)} poll(s), "
            f"{len(saved_blob)} byte blob{OFF}"
        )

        try:
            _run(rep, check_identity, dev, rep)
            _run(rep, check_radio, dev, rep, args.strict_radio)
            if args.factory:
                _run(rep, check_factory_channel, dev, rep)
            else:
                _skip_area(
                    rep, "factory",
                    "--factory not given: this board is not claimed to be a "
                    "shipping image",
                )
            _run(rep, check_config_roundtrip, dev, rep, saved_blob)
            _run(rep, qc_checks.check_blob_robustness, dev, rep)
            _run(rep, qc_checks.check_uplink_routing, dev, rep)
            _run(rep, qc_checks.check_ble_power, dev, rep)

            if rs485_port and _run(rep, check_rs485_echo, dev, rep, rs485_port):
                _run(rep, check_rs485, dev, rep, rs485_port)
                _run(rep, qc_checks.check_link_params, dev, rep, rs485_port)
                _run(rep, qc_checks.check_fc4_and_gap, dev, rep, rs485_port)
                _run(rep, qc_checks.check_corrupt_reply, dev, rep, rs485_port)
                _run(rep, qc_checks.check_interval_change, dev, rep, rs485_port)
                _run(rep, qc_checks.check_idle_mode, dev, rep, rs485_port)
                _run(rep, qc_checks.check_tunnel, dev, rep, rs485_port, args.peer)
            elif rs485_port:
                rep.info("skipping the Modbus checks — the bus itself is not working")
                _skip_area(rep, "rs485", "the bus is not working")
            else:
                _skip_area(rep, "rs485", "--skip-rs485")

            # Reboot-bounded checks go last: everything above is cheaper, and a
            # board that cannot come back should not invalidate results already
            # in hand.
            if args.skip_reboot:
                _skip_area(rep, "reboot", "--skip-reboot")
            else:
                _run(rep, qc_checks.check_persistence, dev, rep)
                _run(rep, qc_checks.check_boot_reliability, dev, rep, args.boot_rounds)

            if args.deep_sleep:
                _run(rep, qc_checks.check_deep_sleep, dev, rep)
            else:
                _skip_area(rep, "role", "not requested (--deep-sleep)")
            if not args.peer:
                _skip_area(rep, "peer", "no second node given (--peer)")
        finally:
            rep.section("cleanup")
            # A release gate must never be able to strand the board it is
            # gating. Whatever failed above, the node goes back to the config it
            # arrived with — and if even that cannot be done, the blob is printed
            # so a human can put it back by hand.
            try:
                dev.quiesce()  # stop it polling first; a busy node is hard to talk to
                status = dev.apply_config(saved_blob)
                rep.check(
                    status == 0,
                    "original configuration restored",
                    f"restore failed: {sq.CONFIG_STATUS.get(status, status)}",
                )
            except DeviceError as exc:
                rep.bad("could not restore the original configuration", str(exc))
                rep.info("restore it by hand with:")
                rep.info(
                    f"  python3 test/hil/restore_config.py --port {device_port} "
                    f"--blob {saved_blob.hex()}"
                )

    elapsed = time.monotonic() - started

    if rep.timings:
        rep.section("timings")
        for name, seconds in rep.timings.items():
            print(f"  {name}: {seconds*1000:.0f} ms")

    rep.checklist()

    meta["elapsed_s"] = elapsed
    if not args.no_report:
        try:
            made = qc_report.write(rep, meta, pathlib.Path(args.report_dir))
            print(f"\n{DIM}report  {made['markdown']}{OFF}")
            if made["pdf"]:
                print(f"{DIM}pdf     {made['pdf']}{OFF}")
            else:
                print(f"{YELLOW}!{OFF} no PDF: {made['pdf_error']}")
        except Exception as exc:  # noqa: BLE001 — recording must not fail the gate
            print(f"{YELLOW}!{OFF} could not write the run report: {exc}")

    if args.json:
        pathlib.Path(args.json).write_text(
            json.dumps(
                {
                    "timings_ms": {
                        k: round(v * 1000, 1) for k, v in rep.timings.items()
                    },
                    "failures": rep.failures,
                    "warnings": rep.warnings,
                    "requirements": rep.status,
                    "notes": rep.notes,
                },
                indent=2,
            )
            + "\n"
        )

    tally = {
        state: sum(1 for v in rep.status.values() if v == state)
        for state in (req.PASS, req.FAIL, req.SKIP, req.NOT_RUN)
    }
    gaps = rep.gaps()

    print(
        f"\n  {tally[req.PASS]} passed, {tally[req.FAIL]} failed, "
        f"{tally[req.SKIP]} skipped, {tally[req.NOT_RUN]} not run "
        f"({len(req.REQUIREMENTS)} requirements, {elapsed:.0f}s)"
    )

    if gaps and not args.allow_gaps:
        # An unexercised requirement is a worse result than a failing one: a
        # failure tells you what is broken, a gap tells you nothing at all while
        # still printing a green run.
        print(
            f"\n{RED}FAILED{OFF}  {len(gaps)} requirement(s) were never exercised — "
            "a release gate that skips a feature is not a gate"
        )
        for requirement in gaps:
            print(f"    {DIM}{requirement.id} {requirement.title}{OFF}")
        print(f"  {DIM}re-run with --allow-gaps to accept this deliberately{OFF}\n")
        return 1

    if rep.failures:
        print(
            f"\n{RED}FAILED{OFF}  {rep.failures} check(s), {rep.warnings} warning(s), {elapsed:.1f}s\n"
        )
        return 1
    print(f"\n{GREEN}PASSED{OFF}  {rep.warnings} warning(s), {elapsed:.1f}s\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
