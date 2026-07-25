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
    def __init__(self):
        self.failures = 0
        self.warnings = 0
        self.timings: dict[str, float] = {}

    def ok(self, msg):
        print(f"  {GREEN}✓{OFF} {msg}")

    def bad(self, msg, detail=None):
        self.failures += 1
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
        print(f"\n{BOLD}{title}{OFF}")


def find_port(patterns, label, flag):
    """Resolve exactly one port, or say what to pick from.

    Guessing is worse than asking here: a Mac typically has several usbmodem
    nodes, and connecting to the wrong one fails several seconds later with a
    protocol timeout that looks like a firmware bug.
    """
    hits = sorted({hit for pattern in patterns for hit in glob.glob(pattern)})
    if not hits:
        raise SystemExit(f"no {label} found (looked for {', '.join(patterns)}) — pass {flag}")
    if len(hits) > 1:
        listing = "\n  ".join(hits)
        raise SystemExit(f"several candidates for the {label}; pick one with {flag}:\n  {listing}")
    return hits[0]


def source_fw_version() -> str | None:
    header = REPO / "src" / "siliqs" / "firmware_core" / "include" / "config.h"
    match = re.search(r'#define\s+SQ_FW_VERSION\s+"([^"]+)"', header.read_text())
    return match.group(1) if match else None


# ── checks ───────────────────────────────────────────────────────────────────


def check_identity(dev: Device, rep: Report):
    rep.section("identity")
    rep.info(f"node 0x{dev.node_num:08x}, firmware {dev.firmware_version}")

    cap = dev.capability()
    rep.info(f"capability: {cap.product_id} fw {cap.firmware}, blob v{cap.max_blob_version}, "
             f"proto {cap.proto}, features 0x{cap.features:02x} ({', '.join(cap.feature_list())})")

    rep.check(cap.product_id == "SQC485Iv2",
              f"product id is {cap.product_id}",
              f"product id is {cap.product_id!r}, expected SQC485Iv2",
              "the configurator labels nodes from this string")

    rep.check(cap.proto == 2, "capability proto 2", f"capability proto {cap.proto}, expected 2")
    rep.check(cap.features == 0x3F,
              f"all six features advertised (0x{cap.features:02x})",
              f"features 0x{cap.features:02x}, expected 0x3F")

    expected_fw = source_fw_version()
    if expected_fw:
        rep.check(cap.firmware == expected_fw,
                  f"reports fw {cap.firmware}, matching the source tree",
                  f"reports fw {cap.firmware}, source says {expected_fw}",
                  "the board is running a different build than this checkout")
    return cap


def check_radio(dev: Device, rep: Report, strict: bool):
    rep.section("radio profile (running config)")
    lora = dev.lora
    complain = rep.bad if strict else rep.warn

    for field, want in CERTIFIED_LORA.items():
        got = getattr(lora, field, None)
        match = abs(got - want) < 1e-6 if isinstance(want, float) else got == want
        if match:
            rep.ok(f"{field} = {got}")
        else:
            complain(f"{field} = {got}, certified value is {want}",
                     "a user may legitimately have changed this; use --strict-radio "
                     "to treat it as a failure (e.g. when verifying a factory image)")

    if dev.bluetooth.enabled:
        rep.warn("bluetooth is enabled",
                 "the factory default for this product is off; expected on a board that "
                 "has been configured by hand, not on a fresh factory image")
    else:
        rep.ok("bluetooth disabled (factory default)")


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

    started = time.monotonic()
    status = dev.apply_config(probe.to_blob())
    rep.timings["config apply"] = time.monotonic() - started

    if not rep.check(status == 0,
                     "config blob accepted",
                     f"config rejected: {sq.CONFIG_STATUS.get(status, status)}"):
        return

    read_back, _ = dev.get_config()
    fields = [
        ("name", probe.name, read_back.name),
        ("baud", probe.baud, read_back.baud),
        ("parity", probe.parity, read_back.parity),
        ("stop_bits", probe.stop_bits, read_back.stop_bits),
        ("response_timeout_ms", probe.response_timeout_ms, read_back.response_timeout_ms),
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
    rep.check(not mismatched,
              "every field survived write → read-back",
              f"{len(mismatched)} field(s) did not survive the round trip",
              "; ".join(f"{n}: wrote {w!r}, read {g!r}" for n, w, g in mismatched))

    for i, (wrote, got) in enumerate(zip(probe.polls, read_back.polls)):
        rep.check(wrote.as_tuple() == got.as_tuple(),
                  f"poll[{i}] round-tripped: slave {got.slave} fc{got.function} "
                  f"@{got.reg_start}×{got.reg_count}",
                  f"poll[{i}] wrote {wrote.as_tuple()}, read {got.as_tuple()}")

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
    rep.info(f"oversized plan: {len(oversized.polls)} polls need "
             f"{oversized.expected_payload_len} bytes, a packet carries {sq.MESH_PAYLOAD_LEN}")
    status = dev.apply_config(oversized.to_blob())
    rep.check(status == 3,
              "an over-sized poll plan is refused (status 3)",
              f"an over-sized plan returned {sq.CONFIG_STATUS.get(status, status)} — "
              "the last polls would be dropped from every uplink with nothing to say so")

    rep.check(dev.apply_config(saved_blob) == 0,
              "device configuration restored",
              "could not restore the original configuration — the board is left on the probe config")


def diagnose_bus(dev: Device, rs485_port: str, baud: int, rep: Report):
    """Separate 'nothing is connected' from 'connected but mis-wired'.

    A silent bus and a polarity-swapped bus fail identically at the Modbus layer,
    but they look completely different on the wire — so make the device transmit
    and dump what the dongle physically hears, rather than guessing.
    """
    import serial as pyserial
    import threading

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
        rep.info("→ no electrical path: check A/B are actually landed, and that the "
                 "device's RS485 transceiver has power")
    elif set(heard) <= {0x00, 0xFF}:
        rep.info(f"the dongle heard {len(heard)} bytes, all {heard[:8].hex()} — "
                 "a continuous break, not data")
        rep.info("→ the line is being driven but cannot be framed. The usual causes are "
                 "A/B swapped, or no common ground between the dongle and the device")
    elif heard.startswith(frame):
        rep.info(f"the dongle heard the request cleanly ({heard[:16].hex()})")
        rep.info("→ wiring is fine; the slave simply did not answer (wrong slave id, or "
                 "the dongle is not in listen mode)")
    else:
        rep.info(f"the dongle heard {len(heard)} bytes: {heard[:32].hex()}")
        rep.info(f"→ garbled: the device sent {frame.hex()}. Check that both ends agree "
                 f"on {baud} 8N1")


def check_rs485_echo(dev: Device, rep: Report, rs485_port: str, baud: int = 9600) -> bool:
    """Prove the bus works before asking anything of the Modbus engine.

    Runs first because a failure here has a much smaller suspect list, and because
    every Modbus symptom downstream would otherwise be explained twice.
    """
    rep.section("RS485 loopback")

    pattern = bytes(range(0x41, 0x49))  # "ABCDEFGH" — printable, easy to spot on a scope

    with SerialEcho(port=rs485_port, baud=baud) as echo:
        time.sleep(0.3)
        echo.clear()
        returned = dev.rs485_bridge(pattern, baud=baud)
        heard = echo.heard

    if heard == pattern:
        rep.ok(f"the dongle heard exactly what the device sent ({heard.hex()})")
    elif heard:
        rep.bad(f"the dongle heard {heard.hex()}, the device sent {pattern.hex()}",
                f"the line is carrying data but it is being mangled — check both ends are at "
                f"{baud} 8N1")
        return False
    else:
        rep.bad("the dongle heard nothing while the device transmitted",
                "the device's TX path or DE (GPIO9, polarity per board rev) is not driving the bus")
        return False

    if returned == pattern:
        rep.ok(f"the device read the echo back ({returned.hex()})")
    elif returned:
        rep.bad(f"the device read back {returned.hex()}, expected {pattern.hex()}",
                "TX works and the echo came back mangled — suspect the TX-echo strip or framing")
        return False
    else:
        rep.bad("the device transmitted but read nothing back",
                "TX works, RX does not: #RE (GPIO2, active low) must be held LOW, and DE must be "
                "released after the write")
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
        deep_sleep=False,        # and keep the node awake for the whole run
    )
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

        if not rep.check(answer.startswith(expect),
                         f"raw bridge reached the slave ({answer.hex()})",
                         f"raw bridge returned {answer.hex()}, expected a frame starting {expect.hex()}"):
            diagnose_bus(dev, rs485_port, plan.baud, rep)
            rep.info("skipping the poll checks — fix the bus first")
            return

        # ── the poll engine ──────────────────────────────────────────────────
        slave.clear_log()
        payload, latency = dev.poll_now()
        rep.timings["poll-now round trip"] = latency

        expected = (
            bytes([0x01, 0x03]) + slave.expected_data(0, 2) +
            bytes([0x01, 0x03]) + slave.expected_data(10, 1)
        )
        rep.check(payload == expected,
                  f"raw-forward payload is byte-exact ({len(payload)} bytes, {latency*1000:.0f} ms)",
                  f"payload {payload.hex()} != expected {expected.hex()}")

        rep.check(len(payload) == plan.expected_payload_len,
                  f"payload length matches the plan ({len(payload)} bytes)",
                  f"payload is {len(payload)} bytes, the plan predicts {plan.expected_payload_len}")

        served = [r for r in slave.requests if r.answered]
        rep.check(len(served) == len(plan.polls),
                  f"slave saw exactly {len(plan.polls)} requests",
                  f"slave saw {len(served)} requests for {len(plan.polls)} polls")

        # Decode the way the cloud does — from the config alone.
        for poll, addr, func, data, error in sq.split_raw_payload(payload, plan.polls):
            label = f"slave {addr} fc{func} @{poll.reg_start}×{poll.reg_count}"
            if error:
                rep.bad(f"{label}: error frame ({error})")
            else:
                rep.ok(f"{label} → {data.hex()}")

        # ── the error path ───────────────────────────────────────────────────
        # A dead slave must still produce a full-length frame, or every later
        # value in the cloud decode shifts.
        slave.drop_requests = True
        slave.clear_log()
        payload, latency = dev.poll_now(timeout=30.0)
        rep.timings["poll-now with dead bus"] = latency

        rep.check(len(payload) == plan.expected_payload_len,
                  f"dead bus still produced a full-length payload ({len(payload)} bytes)",
                  f"dead bus produced {len(payload)} bytes, expected {plan.expected_payload_len}",
                  "a short payload desynchronises the cloud decoder for every following poll")

        decoded = sq.split_raw_payload(payload, plan.polls)
        rep.check(all(err for *_, err in decoded),
                  f"every poll reported an error frame ({decoded[0][-1]})",
                  "a poll reported success against a slave that answered nothing")


# ── entry point ──────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", help="SQC485Iv2 USB console (default: autodetect)")
    ap.add_argument("--rs485", help="USB-RS485 dongle (default: autodetect)")
    ap.add_argument("--skip-rs485", action="store_true", help="console-side checks only")
    ap.add_argument("--strict-radio", action="store_true",
                    help="treat a radio profile deviation as a failure (factory image verification)")
    ap.add_argument("--json", metavar="PATH", help="also write the timings to a JSON file")
    args = ap.parse_args()

    device_port = args.device or find_port(["/dev/cu.usbmodem*"], "SQC485Iv2 console", "--device")
    rs485_port = None
    if not args.skip_rs485:
        # A CH340 dongle enumerates under both the WCH and the generic namespace;
        # prefer the WCH node and never offer the same adapter twice.
        rs485_port = args.rs485 or find_port(
            ["/dev/cu.wchusbserial*"] if glob.glob("/dev/cu.wchusbserial*") else ["/dev/cu.usbserial-*"],
            "USB-RS485 dongle",
            "--rs485",
        )

    print(f"\n{BOLD}SQC485Iv2 hardware in the loop{OFF}")
    print(f"  {DIM}console {device_port}{OFF}")
    print(f"  {DIM}rs485   {rs485_port or 'skipped'}{OFF}")

    rep = Report()
    started = time.monotonic()

    with Device(device_port) as dev:
        saved_cfg, saved_blob = dev.get_config()
        print(f"  {DIM}saved device config: {saved_cfg.name!r}, {len(saved_cfg.polls)} poll(s), "
              f"{len(saved_blob)} byte blob{OFF}")

        try:
            check_identity(dev, rep)
            check_radio(dev, rep, args.strict_radio)
            check_config_roundtrip(dev, rep, saved_blob)
            if rs485_port and check_rs485_echo(dev, rep, rs485_port):
                check_rs485(dev, rep, rs485_port)
            elif rs485_port:
                rep.info("skipping the Modbus checks — the bus itself is not working")
        finally:
            rep.section("cleanup")
            try:
                status = dev.apply_config(saved_blob)
                rep.check(status == 0,
                          "original configuration restored",
                          f"restore failed: {sq.CONFIG_STATUS.get(status, status)}")
            except DeviceError as exc:
                rep.bad("could not restore the original configuration", str(exc))

    elapsed = time.monotonic() - started

    if rep.timings:
        rep.section("timings")
        for name, seconds in rep.timings.items():
            print(f"  {name}: {seconds*1000:.0f} ms")

    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(
            {"timings_ms": {k: round(v * 1000, 1) for k, v in rep.timings.items()},
             "failures": rep.failures, "warnings": rep.warnings}, indent=2) + "\n")

    print()
    if rep.failures:
        print(f"{RED}FAILED{OFF}  {rep.failures} check(s), {rep.warnings} warning(s), {elapsed:.1f}s\n")
        return 1
    print(f"{GREEN}PASSED{OFF}  {rep.warnings} warning(s), {elapsed:.1f}s\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
