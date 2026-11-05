"""
sensor_checks.py — SQS-TH-I-specific QC checks.

Companion to qc_checks.py, which is SQC485Iv2/Modbus-specific. Split into its
own file because the two products share almost nothing below the SQ protocol /
config-blob layer: SQS-TH-I has no RS485, no tunnel, no BLE-power -- just a
built-in HDC1080 sensor and a duty-cycle deep-sleep leaf. See root CLAUDE.md §1
for the product split, and requirements.py's module docstring for how `product`
gates these against the wrong board.

First pass, added 2026-07-31 with a real board on the bench. Deliberately does
NOT cover battery/charging flags (vbat/soc) -- those are still M4 placeholders
in the firmware itself (buildAndSend() sends vbat=0, soc=0xFF unconditionally),
so there is nothing real to verify yet. See QC_RELEASE_READINESS_2026-07-31.md.
"""

from __future__ import annotations

import pathlib
import struct
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "test" / "e2e"))

import sq_protocol as sq  # noqa: E402
from sq_device import Device  # noqa: E402
from sqc485iv2_e2e import firmware_provenance, source_fw_version  # noqa: E402

PORTNUM_258 = 258
PORTNUM_67_NAMES = (67, "TELEMETRY_APP")

# Bench-plausible bounds for the on-board HDC1080 -- a QC policy call, not a
# sensor spec limit (the chip itself covers -40..125C / 0..100%RH). Chosen for
# an indoor bench: 2026-07-31.
SENSOR_TEMP_MIN_C, SENSOR_TEMP_MAX_C = 20.0, 33.0
SENSOR_RH_MIN_PCT, SENSOR_RH_MAX_PCT = 10.0, 90.0

# SQS-TH-I's sensor feature set (config.h): POLL_NOW|DEEP_SLEEP|GET_CONFIG|SENSOR.
# Must NOT include TUNNEL/BLE_POWER/RS485_TERM -- this product has none of those.
SENSOR_EXPECTED_FEATURES = 0x08 | 0x10 | 0x20 | 0x40


def check_identity_sensor(dev: Device, rep):
    """SENSOR-ID-01/02, SENSOR-CAP-01 — SQS-TH-I's identity/capability handshake."""
    rep.section("identity (SQS-TH-I)")
    rep.info(f"node 0x{dev.node_num:08x}, firmware {dev.firmware_version}")

    cap = dev.capability()
    rep.info(
        f"capability: {cap.product_id} fw {cap.firmware}, blob v{cap.max_blob_version}, "
        f"proto {cap.proto}, features 0x{cap.features:02x} ({', '.join(cap.feature_list())})"
    )

    rep.requirement("SENSOR-ID-01")
    rep.check(
        cap.product_id == "SQS-TH-I",
        f"product id is {cap.product_id}",
        f"product id is {cap.product_id!r}, expected SQS-TH-I",
    )

    rep.requirement("SENSOR-ID-02")
    expected_fw = source_fw_version(REPO)
    if expected_fw:
        rep.check(
            cap.firmware == expected_fw,
            f"reports fw {cap.firmware}, matching the source tree",
            f"reports fw {cap.firmware}, source says {expected_fw}",
            "the board is running a different build than this checkout",
        )
    ok, why = firmware_provenance(REPO, dev.firmware_version)
    if ok is None:
        rep.info(f"provenance not checked: {why}")
    else:
        rep.check(ok, why, why, "SQ_FW_VERSION agreeing is not enough on its own")

    rep.requirement("SENSOR-CAP-01")
    rep.check(
        cap.features == SENSOR_EXPECTED_FEATURES,
        f"sensor feature set advertised (0x{cap.features:02x})",
        f"features 0x{cap.features:02x}, expected 0x{SENSOR_EXPECTED_FEATURES:02x}",
        "the configurator hides the Modbus UI based on the SENSOR bit alone; a "
        "wrong feature set breaks that auto-detect silently",
    )
    return cap


def check_sensor_reading(dev: Device, rep, watch_s: int = 100):
    """SENSOR-READ-01/02 — watch the mesh for a real HDC1080 reading and standard
    EnvironmentMetrics, without holding the connection in a way that would starve
    the watch (both come from the periodic report, not a request/reply)."""
    rep.section(f"sensor reading (watching {watch_s}s)")

    seen: dict = {}

    def on_receive(packet=None, interface=None):
        decoded = packet.get("decoded") or {}
        portnum = decoded.get("portnum")
        raw = decoded.get("payload")
        if raw is None:
            return
        if portnum in PORTNUM_67_NAMES:
            seen.setdefault(67, []).append(raw)
        elif portnum == PORTNUM_258 and not (len(raw) >= 3 and raw[:3] in (b"SQV", b"SQG", b"SQ!")):
            # PortNum 258 also carries SQV?/SQG?/config-write replies -- a real
            # health packet is 11 bytes starting with ver=1, not an ASCII marker.
            seen.setdefault(258, []).append(raw)

    from pubsub import pub

    pub.subscribe(on_receive, "meshtastic.receive")
    try:
        deadline = time.time() + watch_s
        while time.time() < deadline and (67 not in seen or 258 not in seen):
            time.sleep(0.5)
    finally:
        pub.unsubscribe(on_receive, "meshtastic.receive")

    rep.requirement("SENSOR-READ-01")
    if rep.check(
        258 in seen,
        f"saw a PortNum 258 (Siliqs health) reading ({len(seen.get(258, []))} in window)",
        f"no PortNum 258 reading in {watch_s}s",
        "the report cadence is config-driven; a node quiesced to a long interval "
        "just before this check would legitimately miss the window",
    ):
        raw = seen[258][-1]
        if len(raw) >= 11:
            ver, model, temp, rh, vbat, soc, flags, boot = struct.unpack("<BBhHHBBB", raw[:11])
            temp_c, rh_pct = temp / 100.0, rh / 100.0
            rep.info(
                f"ver={ver} model={model} T={temp_c:.2f}C RH={rh_pct:.2f}% "
                f"vbat={vbat} soc=0x{soc:02x} flags=0x{flags:02x} boot={boot}"
            )
            rep.check(
                not (flags & 0x01),
                "sensor NACK flag is clear",
                f"sensor NACK flag is set (flags=0x{flags:02x}) -- the HDC1080 "
                "read failed",
            )
            rep.check(
                SENSOR_TEMP_MIN_C <= temp_c <= SENSOR_TEMP_MAX_C
                and SENSOR_RH_MIN_PCT <= rh_pct <= SENSOR_RH_MAX_PCT,
                f"reading is bench-plausible (T={temp_c:.2f}C RH={rh_pct:.2f}%)",
                f"reading is outside bench-plausible bounds (T={temp_c:.2f}C RH={rh_pct:.2f}%, "
                f"expected {SENSOR_TEMP_MIN_C}-{SENSOR_TEMP_MAX_C}C / "
                f"{SENSOR_RH_MIN_PCT}-{SENSOR_RH_MAX_PCT}%RH)",
                "either the sensor is misreading or the bench itself is out of "
                "the expected range -- check the room before assuming firmware",
            )
        else:
            rep.bad(f"PortNum 258 payload is {len(raw)} bytes, expected >= 11")

    rep.requirement("SENSOR-READ-02")
    rep.check(
        67 in seen,
        f"saw a PortNum 67 (EnvironmentMetrics) reading ({len(seen.get(67, []))} in window)",
        f"no PortNum 67 reading in {watch_s}s",
        "sendEnvironmentMetrics() only fires when the HDC1080 read succeeded -- "
        "if 258 shows a NACK, expect this to be empty too",
    )


def check_sensor_deep_sleep(dev: Device, rep, interval_s: int = 30):
    """SENSOR-SLEEP-01 — the duty-cycle leaf actually powers down and comes back.

    Mirrors qc_checks.check_deep_sleep (SQC485Iv2's FEAT-03) closely: same
    invasive-write-then-disconnect-and-watch-the-port shape, same generous
    timeout budget for the same reason -- SensorTHModule's connect-grace
    constant (SQS_CONNECT_GRACE_MS, 30s) is fixed in firmware and not
    configurable via the blob, so a tight timeout races a constant it cannot
    win against. Unlike FEAT-03, this does NOT need needs="role" to actually
    change anything -- SQS-TH-I already factory-defaults to CLIENT_MUTE
    (NodeDB.cpp installDefaultConfig()) -- but it still writes config and
    reboots the board, which is exactly the class of check that tag exists to
    gate behind an explicit opt-in.
    """
    rep.section(f"duty-cycle deep sleep ({interval_s}s interval)")

    cfg = sq.Config(
        name="qc-sleep",
        polls=[],
        rs485_enabled=False,
        uplink_interval_s=interval_s,
        deep_sleep=True,
    )
    if not rep.check(
        dev.apply_config(cfg.to_blob()) == 0,
        "deep-sleep config accepted",
        "the node refused the deep-sleep config",
    ):
        return

    rep.info("disconnecting so the node is free to sleep")
    port = dev.port
    dev.close()

    import qc_checks  # local import: only needed here, avoids a module-load-order tangle

    vanished = qc_checks._wait_for_port(port, present=False, timeout=interval_s + 90)
    rep.requirement("SENSOR-SLEEP-01")
    rep.check(
        vanished,
        "the node powered down (USB CDC disappeared)",
        f"the port never went away within {interval_s + 90}s — the leaf is not "
        "entering deep sleep, so a battery unit would run flat",
    )
    came_back = qc_checks._wait_for_port(port, present=True, timeout=interval_s + 90)
    rep.check(
        came_back,
        "the node woke on its timer (USB CDC came back)",
        "the node did not come back — a leaf that fails to wake is a dead node "
        "in the field",
    )

    qc_checks._reconnect(dev)
