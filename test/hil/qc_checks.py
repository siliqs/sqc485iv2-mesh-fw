"""
qc_checks.py — the acceptance checks that close the gaps in the original run.

sq_hil.py covers identity, the radio profile, a config round trip and the happy
+ error paths of the poll engine. These are the rest of the list in
requirements.py: the blob contract's rejection paths, persistence across a
reboot, the wire-level link settings, the operating modes, and the three
advertised features (tunnel, BLE power, deep sleep) that nothing exercised.

They are separate from sq_hil.py only because that file is already long; they
follow the same shape — take a Device and a Report, declare the requirement they
answer, leave the board no worse than they found it.
"""

from __future__ import annotations

import pathlib
import struct
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import sq_protocol as sq  # noqa: E402
from modbus_slave import ModbusSlave, crc16  # noqa: E402
from sq_device import Device, DeviceError  # noqa: E402


def _idle(name="qc-idle", **kw):
    """A config that keeps the node off the bus and out of the way."""
    kw.setdefault("polls", [])
    kw.setdefault("rs485_enabled", False)
    kw.setdefault("uplink_interval_s", 3600)
    kw.setdefault("deep_sleep", False)
    return sq.Config(name=name, **kw)


# ── config blob contract ─────────────────────────────────────────────────────


def check_blob_robustness(dev: Device, rep):
    """CFG-04 / CFG-05 — what the node does with blobs it should not accept."""
    rep.section("config blob — rejection and back-compat")
    dev.quiesce()

    good = _idle("qc-blob").to_blob()

    # Each of these is a distinct way a blob goes wrong in the field: a truncated
    # BLE write, a bit flipped in flight, a configurator built against a newer
    # schema. All of them must NAK — applying half of one is how a node ends up
    # polling a plan nobody chose.
    corrupt_crc = bytearray(good)
    corrupt_crc[-1] ^= 0xFF
    bad_magic = bytearray(good)
    bad_magic[0] = ord("X")
    bad_version = bytearray(good)
    bad_version[2] = 15  # in the 2..15 "this is a config" window, but unknown

    cases = [
        ("a blob with a broken CRC", bytes(corrupt_crc)),
        ("a truncated blob", good[: len(good) // 2]),
        ("a blob claiming an unknown version", bytes(bad_version)),
    ]

    rep.requirement("CFG-04")
    for label, blob in cases:
        try:
            status = dev.apply_config(blob, attempts=1, timeout=8.0)
        except DeviceError:
            # No SQ! at all is also a rejection, but a silent one: the
            # configurator cannot distinguish it from a lost packet.
            rep.bad(
                f"{label} was not acknowledged at all",
                "the node must answer 'SQ!' with status 1 so the UI can say the "
                "write was refused rather than showing 'unconfirmed'",
            )
            continue
        rep.check(
            status == 1,
            f"{label} was rejected (status 1)",
            f"{label} returned {sq.CONFIG_STATUS.get(status, status)}, expected 1",
            "a blob that fails validation must never be applied",
        )

    # A bad magic byte is not a config write at all — the module ignores anything
    # that is not 'SQ'+version, so silence is the correct answer here, not a NAK.
    try:
        status = dev.apply_config(bytes(bad_magic), attempts=1, timeout=5.0)
        rep.bad(
            f"a blob with the wrong magic returned {sq.CONFIG_STATUS.get(status, status)}",
            "packets that are not 'SQ' payloads must be ignored, not answered — "
            "otherwise every unrelated packet on the portnum draws a reply",
        )
    except DeviceError:
        rep.ok("a blob with the wrong magic is ignored (no reply)")

    # Configurators in the field are not upgraded in lockstep with firmware, so
    # the older blob layouts have to keep working.
    rep.requirement("CFG-05")
    for version in (2, 3):
        legacy = _idle(f"qc-v{version}", polls=[sq.Poll(1, 3, 0, 1)])
        legacy.version = version
        try:
            status = dev.apply_config(legacy.to_blob(), attempts=1, timeout=8.0)
        except DeviceError as exc:
            rep.bad(f"a v{version} blob was not acknowledged", str(exc))
            continue
        if not rep.check(
            status == 0,
            f"a v{version} blob is still accepted",
            f"a v{version} blob returned {sq.CONFIG_STATUS.get(status, status)}",
            "config.h promises v2/v3 remain readable",
        ):
            continue
        read_back, _ = dev.get_config()
        rep.check(
            len(read_back.polls) == 1 and read_back.polls[0].as_tuple() == (1, 3, 0, 1),
            f"the v{version} poll plan survived the upgrade to v{sq.Config().version}",
            f"v{version} plan read back as {[p.as_tuple() for p in read_back.polls]}",
        )


def check_persistence(dev: Device, rep):
    """CFG-06 — the blob lives in LittleFS; prove it is still there after a reset."""
    rep.section("persistence across a reboot")
    rep.requirement("CFG-06")

    marker = _idle("qc-persist", polls=[sq.Poll(5, 4, 77, 2)], baud=19200, retries=1)
    dev.quiesce()
    if not rep.check(
        dev.apply_config(marker.to_blob()) == 0,
        "marker config written",
        "could not write the marker config",
    ):
        return

    try:
        elapsed = dev.reboot()
    except DeviceError as exc:
        rep.bad("the node did not come back from a reboot", str(exc))
        return
    rep.info(f"back on the API {elapsed:.1f}s after the reboot request")

    read_back, _ = dev.get_config()
    same = (
        read_back.name == marker.name
        and read_back.baud == marker.baud
        and read_back.retries == marker.retries
        and [p.as_tuple() for p in read_back.polls]
        == [p.as_tuple() for p in marker.polls]
    )
    rep.check(
        same,
        "the configuration survived the reboot",
        f"after the reboot the node reports name={read_back.name!r} "
        f"baud={read_back.baud} plan={[p.as_tuple() for p in read_back.polls]}",
        "the blob is not reaching LittleFS, or is not being read back at boot — "
        "every field unit would come up on defaults after a power cut",
    )


def check_boot_reliability(dev: Device, rep, rounds: int = 3):
    """BOOT-01 — this board's historical failure mode is a brownout boot loop."""
    rep.section(f"boot reliability ({rounds} resets)")
    rep.requirement("BOOT-01")

    times = []
    for i in range(rounds):
        try:
            elapsed = dev.reboot()
        except DeviceError as exc:
            rep.bad(
                f"the node did not come back from reset {i + 1} of {rounds}",
                f"{exc} — the brownout mitigation in main.cpp is load-bearing on "
                "this hardware revision (no bulk decoupling on 3.3V)",
            )
            return
        times.append(elapsed)
        rep.info(f"reset {i + 1}: back on the API in {elapsed:.1f}s")

    rep.ok(f"came back from all {rounds} resets (worst {max(times):.1f}s)")
    # A board that is brownout-marginal does not fail cleanly, it takes several
    # tries to enumerate — so a big spread is a warning even when nothing failed.
    if max(times) > 2 * min(times) + 3:
        rep.warn(
            f"boot time varies a lot ({min(times):.1f}s … {max(times):.1f}s)",
            "uneven bring-up on this board usually means the 3.3V rail is "
            "marginal during the RF transient, not that the firmware is slow",
        )


# ── wire-level link settings ─────────────────────────────────────────────────


def check_link_params(dev: Device, rep, rs485_port: str):
    """BUS-04 / POLL-11 — baud is honoured, by the bridge and by the poll engine."""
    rep.section("link parameters on the wire")

    # 19200 is far enough from the 9600 default that a device ignoring the
    # setting produces framing garbage rather than a lucky partial match.
    baud = 19200
    plan = sq.Config(
        name="qc-baud",
        polls=[sq.Poll(1, 3, 0, 1)],
        baud=baud,
        response_timeout_ms=1000,
        retries=1,
        poll_gap_ms=20,
        uplink_interval_s=3600,
        deep_sleep=False,
    )
    dev.quiesce()

    slave = ModbusSlave(port=rs485_port, slave_id=1, baud=baud, registers={0: 0x5A5A})
    with slave:
        time.sleep(0.3)

        # The bridge carries its own link header, so it must reach a 19200 slave
        # while the stored polling config still says 9600.
        rep.requirement("BUS-04")
        frame = bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x01])
        frame += crc16(frame).to_bytes(2, "little")
        try:
            answer = dev.rs485_bridge(frame, baud=baud)
        except DeviceError as exc:
            answer = b""
            rep.info(f"bridge did not reply: {exc}")
        rep.check(
            answer.startswith(bytes([0x01, 0x03, 0x02]) + b"\x5a\x5a"),
            f"the bridge honoured its own {baud} baud header ({answer.hex()})",
            f"bridge at {baud} returned {answer.hex() or '(nothing)'} — the request's "
            "link header is being ignored in favour of the stored config",
        )

        # And a config push must re-init the UART, without a power cycle.
        rep.requirement("POLL-11")
        if dev.apply_config(plan.to_blob()) != 0:
            rep.bad("could not install the 19200 poll plan")
            return
        slave.clear_log()
        try:
            payload, _ = dev.poll_now(timeout=20.0)
        except DeviceError as exc:
            rep.bad("no telemetry after the baud change", str(exc))
            return
        rep.check(
            payload == bytes([0x01, 0x03, 0x5A, 0x5A]),
            f"the poll engine moved to {baud} without a reboot ({payload.hex()})",
            f"payload {payload.hex()} — the UART did not follow the config change, "
            "so every on-site baud correction would need a power cycle",
        )


def check_fc4_and_gap(dev: Device, rep, rs485_port: str):
    """POLL-04 / POLL-05 — input registers on the wire, and the settle delay."""
    rep.section("function code 4 and the poll gap")

    gap_ms = 250  # big enough to measure against USB and thread jitter
    plan = sq.Config(
        name="qc-fc4",
        polls=[sq.Poll(1, 4, 20, 2), sq.Poll(1, 4, 30, 1)],
        baud=9600,
        response_timeout_ms=1000,
        retries=1,
        poll_gap_ms=gap_ms,
        uplink_interval_s=3600,
        deep_sleep=False,
    )
    dev.quiesce()
    if dev.apply_config(plan.to_blob()) != 0:
        rep.bad("could not install the fc4 plan")
        return

    slave = ModbusSlave(
        port=rs485_port,
        slave_id=1,
        baud=plan.baud,
        registers={20: 0x1111, 21: 0x2222, 30: 0x3333},
    )
    with slave:
        time.sleep(0.3)
        slave.clear_log()
        try:
            payload, _ = dev.poll_now(timeout=20.0)
        except DeviceError as exc:
            rep.bad("no telemetry from the fc4 plan", str(exc))
            return

        rep.requirement("POLL-04")
        expected = (
            bytes([0x01, 0x04])
            + slave.expected_data(20, 2)
            + bytes([0x01, 0x04])
            + slave.expected_data(30, 1)
        )
        rep.check(
            payload == expected,
            f"input registers (fc4) read and forwarded byte-exact ({payload.hex()})",
            f"fc4 payload {payload.hex()} != expected {expected.hex()}",
            "half the field devices only expose input registers",
        )
        seen = [r.function for r in slave.requests]
        rep.check(
            seen and all(f == 4 for f in seen),
            f"the slave saw function code 4 ({len(seen)} request(s))",
            f"the slave saw function codes {seen}, expected all 4",
        )

        rep.requirement("POLL-05")
        stamps = [r.at for r in slave.requests]
        if len(stamps) < 2:
            rep.bad(
                f"only {len(stamps)} request(s) reached the slave — cannot measure the gap"
            )
        else:
            measured = (stamps[1] - stamps[0]) * 1000
            rep.check(
                measured >= gap_ms * 0.7,
                f"poll_gap honoured: {measured:.0f} ms between polls (configured {gap_ms})",
                f"only {measured:.0f} ms between polls, configured {gap_ms} — the "
                "settle delay is not being applied, so a slow transceiver can hear "
                "the next request as a tail of the previous reply",
            )


def check_corrupt_reply(dev: Device, rep, rs485_port: str):
    """POLL-10 — a reply that fails CRC must not be forwarded as data."""
    rep.section("a corrupt reply")
    rep.requirement("POLL-10")

    plan = sq.Config(
        name="qc-crc",
        polls=[sq.Poll(1, 3, 0, 2)],
        baud=9600,
        response_timeout_ms=1000,
        retries=0,
        poll_gap_ms=20,
        uplink_interval_s=3600,
        deep_sleep=False,
    )
    dev.quiesce()
    if dev.apply_config(plan.to_blob()) != 0:
        rep.bad("could not install the CRC test plan")
        return

    slave = ModbusSlave(
        port=rs485_port, slave_id=1, baud=plan.baud, registers={0: 0xDEAD, 1: 0xBEEF}
    )
    slave.corrupt_crc = True
    with slave:
        time.sleep(0.3)
        slave.clear_log()
        try:
            payload, _ = dev.poll_now(timeout=20.0)
        except DeviceError as exc:
            rep.bad("no telemetry against a corrupting slave", str(exc))
            return

        decoded = sq.split_raw_payload(payload, plan.polls)
        rep.check(
            len(payload) == plan.expected_payload_len,
            f"a corrupt reply still produced a full-length payload ({len(payload)} bytes)",
            f"payload was {len(payload)} bytes, plan predicts {plan.expected_payload_len}",
        )
        rep.check(
            all(r.error for r in decoded),
            f"the corrupt reply was rejected ({decoded[0].error})",
            f"payload {payload.hex()} was forwarded as a reading — a CRC failure "
            "that reaches the cloud as data is worse than no reading at all",
        )


# ── operating modes ──────────────────────────────────────────────────────────


def check_interval_change(dev: Device, rep, rs485_port: str):
    """CFG-07 — shortening the reporting interval has to take effect now, not later.

    applyConfigBlob() re-inits the UART and returns; it never touches the thread's
    schedule. So if runOnce() has just returned `uplink_interval_s * 1000`, the
    node is asleep for that long and a new, shorter interval cannot start until
    the OLD one expires. This is the check for the field case that looks like a
    dead node: move a unit from hourly to minutely reporting, and watch nothing
    happen for an hour.
    """
    rep.section("a shortened uplink interval")
    rep.requirement("CFG-07")

    long_s, short_s, grace_s = 25, 4, 12
    common = dict(
        polls=[sq.Poll(1, 3, 0, 1)],
        baud=9600,
        response_timeout_ms=500,
        retries=0,
        deep_sleep=False,
    )
    slave = ModbusSlave(port=rs485_port, slave_id=1, baud=9600, registers={0: 0x1234})
    with slave:
        dev.quiesce()
        if dev.apply_config(
            sq.Config(name="qc-int-long", uplink_interval_s=long_s, **common).to_blob()
        ) != 0:
            rep.bad("could not install the long-interval plan")
            return
        dev.reboot()  # start the timer from this config

        # Let it settle into the long cadence, then shorten it.
        time.sleep(short_s + 2)
        if dev.apply_config(
            sq.Config(name="qc-int-short", uplink_interval_s=short_s, **common).to_blob()
        ) != 0:
            rep.bad("could not install the short-interval plan")
            return

        slave.clear_log()
        started = time.monotonic()
        deadline = started + long_s + 10
        while time.monotonic() < deadline and not slave.requests:
            time.sleep(0.05)
        if not slave.requests:
            rep.bad(
                f"no poll at all within {long_s + 10}s of shortening the interval"
            )
            return
        waited = slave.requests[0].at - started
        rep.check(
            waited <= grace_s,
            f"the shortened interval took effect in {waited:.1f}s (asked for {short_s}s)",
            f"the next poll came {waited:.1f}s after the change, not ~{short_s}s — the "
            f"node sat out the old {long_s}s interval first. A unit moved from hourly "
            "to minutely reporting stays silent for up to an hour and reads as dead",
        )


def check_idle_mode(dev: Device, rep, rs485_port: str, window_s: float = 14.0):
    """MODE-01 — rs485_enabled=false is the gateway mode: no PERIODIC bus traffic.

    Deliberately does not use 'SQ?'. Poll-now is an explicit operator request and
    handleReceived() runs it regardless of the flag — which is correct, the user
    asked. What the flag governs is the timer in runOnce(), so the only honest way
    to test it is to watch the bus over a couple of intervals and see nothing.
    """
    rep.section("rs485_enabled = false (gateway mode)")
    rep.requirement("MODE-01")

    interval = 4  # short enough that a couple of cycles fit in the window
    slave = ModbusSlave(port=rs485_port, slave_id=1, baud=9600, registers={0: 0x1234})
    with slave:
        # Prove periodic polling is running first, or "no requests" proves nothing.
        active = sq.Config(
            name="qc-idle-on",
            polls=[sq.Poll(1, 3, 0, 1)],
            baud=9600,
            response_timeout_ms=500,
            retries=0,
            uplink_interval_s=interval,
            deep_sleep=False,
        )
        dev.quiesce()
        if dev.apply_config(active.to_blob()) != 0:
            rep.bad("could not install the control plan")
            return
        # A config push does not reschedule the poll thread (see CFG-07), so a
        # node that last returned a long interval is still asleep on it. Reboot
        # so the timer starts from this config, or the control step below would
        # measure the previous plan's schedule.
        dev.reboot()
        time.sleep(0.5)
        slave.clear_log()
        time.sleep(window_s)
        polled = len(slave.requests)
        if not rep.check(
            polled > 0,
            f"control: the node polls on its timer ({polled} request(s) in {window_s:.0f}s)",
            f"no periodic requests in {window_s:.0f}s even with RS485 enabled — fix "
            "that before reading anything into the idle test",
        ):
            return

        # Now turn it off and watch the same window stay empty.
        off = _idle("qc-idle-off", polls=[sq.Poll(1, 3, 0, 1)])
        off.uplink_interval_s = interval
        if dev.apply_config(off.to_blob()) != 0:
            rep.bad("could not disable RS485")
            return
        time.sleep(0.5)
        slave.clear_log()
        time.sleep(window_s)
        rep.check(
            len(slave.requests) == 0,
            f"with rs485_enabled=false the bus stayed quiet for {window_s:.0f}s",
            f"the slave still saw {len(slave.requests)} periodic request(s) with "
            "RS485 disabled — a gateway would keep waking and logging errors "
            "against a bus that has nothing on it",
        )


def check_uplink_routing(dev: Device, rep):
    """MODE-02 / MODE-03 — where the uplink is addressed, and whether it asks for an ACK.

    Both are config fields the round trip already proves are STORED. What has
    never been checked is that they change the packet the node actually emits.
    """
    rep.section("uplink routing (dest_node / confirmed)")

    seen: list[dict] = []

    def watch(packet):
        decoded = packet.get("decoded") or {}
        payload = bytes(decoded.get("payload", b""))
        if payload[:2] == b"SQ":
            return  # our own command/reply markers, not telemetry
        seen.append(packet)

    dev.add_listener(watch)

    # Address the uplink at this node itself. A made-up destination would prove
    # the same field is honoured, but an unroutable unicast can be dropped before
    # the copy that reaches the attached client — so the check would fail for a
    # reason that has nothing to do with what it is testing. What matters here is
    # that `to` stops being the broadcast address.
    dest = dev.node_num
    plan = sq.Config(
        name="qc-route",
        polls=[sq.Poll(1, 3, 0, 1)],
        rs485_enabled=True,
        baud=9600,
        response_timeout_ms=250,  # nothing need answer; keep the cycle short
        retries=0,
        uplink_interval_s=3600,
        deep_sleep=False,
        tx_dest=dest,
        tx_channel=0,
        confirmed=True,
    )
    dev.quiesce()
    if dev.apply_config(plan.to_blob()) != 0:
        rep.bad("could not install the routing plan")
        return

    seen.clear()
    try:
        dev.poll_now(timeout=20.0)
    except DeviceError as exc:
        rep.bad("no telemetry to inspect", str(exc))
        return
    time.sleep(0.5)

    if not seen:
        rep.bad("the uplink was not visible on the API — cannot inspect its routing")
        return
    packet = seen[-1]

    rep.requirement("MODE-02")
    to = packet.get("to")
    rep.check(
        to == dest and to != 0xFFFFFFFF,
        f"the uplink is unicast to 0x{dest:08x}, not broadcast",
        f"the uplink went to {to if to is None else hex(to)}, expected 0x{dest:08x} — "
        "a deployment routing telemetry to one collector would be flooding the mesh",
    )

    rep.requirement("MODE-03")
    want_ack = bool(packet.get("wantAck"))
    rep.check(
        want_ack,
        "the confirmed uplink carries want_ack",
        "want_ack was not set on a confirmed uplink — Level-3 delivery degrades to "
        "a broadcast that is merely shouted, with no way to know it landed",
    )


# ── advertised features with no other coverage ───────────────────────────────


def check_ble_power(dev: Device, rep):
    """FEAT-01 — 'SQ P' is advertised (bit 0x02) and nothing has ever sent one."""
    rep.section("'SQ P' BLE transmit power")
    rep.requirement("FEAT-01")

    dev.quiesce()
    # There is no read-back for this value: it goes to its own store key, not the
    # config blob. What can be shown is that the node accepts the command, keeps
    # serving the protocol afterwards, and still has its config — i.e. the write
    # did not fault or clobber the blob.
    before, _ = dev.get_config()
    for dbm in (-3, 9):
        dev.send(sq.CMD_BLE_POWER + struct.pack("b", dbm))
        time.sleep(0.4)

    try:
        cap = dev.capability()
    except DeviceError as exc:
        rep.bad("the node stopped answering after 'SQ P'", str(exc))
        return
    rep.check(
        cap.features & 0x02,
        "the node still answers after two 'SQ P' writes",
        "the node answered but no longer advertises the ble_power feature",
    )
    after, _ = dev.get_config()
    rep.check(
        after.name == before.name and len(after.polls) == len(before.polls),
        "'SQ P' left the config blob untouched (separate store key)",
        f"the config changed across an 'SQ P' write: {before.name!r} -> {after.name!r}",
        "BLE power is persisted to its own key; touching the config blob would "
        "mean an unrelated command can corrupt the poll plan",
    )
    rep.info(
        "no read-back exists for this value — the check proves it is accepted and "
        "isolated, not that the radio power actually changed"
    )


def check_tunnel(dev: Device, rep, rs485_port: str, peer_port: str | None = None):
    """FEAT-02 — the RS485↔RS485 tunnel, with the far end played in software.

    This one genuinely needs two nodes, and the reason is worth writing down:
    forwardTunnel() sends with ccToPhone=false —

        service->sendToMesh(p, RX_SRC_LOCAL, false);   // unicast to the peer

    so unlike telemetry, the forwarded frame is never copied to the attached
    client. A single-node bench cannot observe it at all, no matter how the peer
    is faked; the frame only exists on the air. Rather than assert something
    weaker and call the feature covered, this reports SKIP unless a second
    Meshtastic node is given with --peer, and FakePeer runs on that.
    """
    rep.section("RS485↔RS485 tunnel")

    if not peer_port:
        rep.skip(
            "FEAT-02",
            "needs a second node (--peer): forwardTunnel() sends with "
            "ccToPhone=false, so the forwarded frame never reaches this client",
        )
        rep.info(
            "skipped — the tunnel forward is air-only (ccToPhone=false), so it "
            "cannot be observed from the master's own API. Pass --peer <port> "
            "with a second Meshtastic node to run this."
        )
        return

    rep.requirement("FEAT-02")
    import serial as pyserial

    from fake_peer import FakePeer  # local: it subscribes on construction

    peer_dev = Device(peer_port)
    peer_dev.connect()
    try:
        cfg = sq.Config(
            name="qc-tunnel",
            polls=[],
            rs485_enabled=True,
            tunnel_enabled=True,
            tunnel_peer=peer_dev.node_num,
            tunnel_idle_gap=30,
            baud=9600,
            uplink_interval_s=3600,
            deep_sleep=False,
        )
        dev.quiesce()
        if not rep.check(
            dev.apply_config(cfg.to_blob()) == 0,
            "tunnel config accepted",
            "the node refused a tunnel config",
        ):
            return

        frame = bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x01])
        frame += crc16(frame).to_bytes(2, "little")

        with FakePeer(peer_dev, registers={0: 0xC0DE}) as peer, pyserial.Serial(
            rs485_port, 9600, timeout=1.0
        ) as bus:
            time.sleep(0.5)
            bus.reset_input_buffer()
            peer.clear()
            bus.write(frame)  # a frame appearing on the master's local bus
            bus.flush()

            deadline = time.time() + 20.0
            back = b""
            while time.time() < deadline:
                back += bus.read(64)
                if peer.seen("tunnel") and len(back) >= 7:
                    break

            forwarded = peer.seen("tunnel")
            if not rep.check(
                forwarded > 0,
                f"the master forwarded the local frame to its peer ({forwarded} exchange(s))",
                "the peer never saw the frame — the tunnel master is not forwarding "
                "what appears on its local bus",
            ):
                return
            rep.check(
                back.startswith(bytes([0x01, 0x03, 0x02])),
                f"the peer's reply was written back to the local bus ({back.hex()})",
                f"the local bus got {back.hex() or '(nothing)'} back — the reply path "
                "('SQ{' → local RS485) is not closing the loop",
            )
    finally:
        peer_dev.close()


def check_deep_sleep(dev: Device, rep, interval_s: int = 30):
    """FEAT-03 — the duty-cycle leaf actually powers down and comes back.

    Invasive: deep sleep only engages for a CLIENT_MUTE node, so this changes the
    Meshtastic role and puts it back. Opt-in for that reason.
    """
    rep.section(f"duty-cycle deep sleep ({interval_s}s interval)")
    rep.requirement("FEAT-03")

    node = dev.iface.localNode
    original_role = node.localConfig.device.role
    try:
        cfg = sq.Config(
            name="qc-sleep",
            polls=[sq.Poll(1, 3, 0, 1)],
            rs485_enabled=True,
            baud=9600,
            response_timeout_ms=250,
            retries=0,
            uplink_interval_s=interval_s,
            deep_sleep=True,
        )
        dev.quiesce()
        if not rep.check(
            dev.apply_config(cfg.to_blob()) == 0,
            "deep-sleep config accepted",
            "the node refused the deep-sleep config",
        ):
            return

        # By name, not by number: the role enum has grown several times upstream
        # and a hard-coded 1 silently becomes the wrong role after a protobuf bump.
        from meshtastic.protobuf import config_pb2

        node.localConfig.device.role = config_pb2.Config.DeviceConfig.Role.Value(
            "CLIENT_MUTE"
        )
        node.writeConfig("device")
        rep.info("role set to CLIENT_MUTE; disconnecting so the node is free to sleep")

        # While an API client is attached the node deliberately stays awake, so the
        # only way to observe the duty cycle is to let go of the port.
        port = dev.port
        dev.close()
        vanished = _wait_for_port(port, present=False, timeout=interval_s + 60)
        rep.check(
            vanished,
            "the node powered down (USB CDC disappeared)",
            f"the port never went away within {interval_s + 60}s — the leaf is not "
            "entering deep sleep, so a battery unit would run flat",
        )
        came_back = _wait_for_port(port, present=True, timeout=interval_s + 60)
        rep.check(
            came_back,
            "the node woke on its timer (USB CDC came back)",
            "the node did not come back — a leaf that fails to wake is a dead node "
            "in the field",
        )
    finally:
        # Getting the role back matters more than any result above: a board left
        # on CLIENT_MUTE + deep_sleep only shows a 2-4 s window per cycle.
        _reconnect(dev)
        try:
            dev.iface.localNode.localConfig.device.role = original_role
            dev.iface.localNode.writeConfig("device")
            rep.ok("device role restored")
        except Exception as exc:  # noqa: BLE001
            rep.bad(
                "could not restore the device role",
                f"{exc} — the board is on CLIENT_MUTE and will keep duty-cycling; "
                "catch it during a wake window to put it back",
            )


def _wait_for_port(port: str, present: bool, timeout: float) -> bool:
    """Poll fast: a waking node offers only a few seconds before it sleeps again."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pathlib.Path(port).exists() == present:
            return True
        time.sleep(0.03)
    return False


def _reconnect(dev: Device, attempts: int = 40) -> None:
    for _ in range(attempts):
        try:
            dev.connect(attempts=1)
            return
        except DeviceError:
            time.sleep(0.5)
