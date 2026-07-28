#!/opt/homebrew/bin/python3
"""End-to-end bench test for a live SQC485Iv2 node over its USB console.

Exercises the whole product contract against real hardware, in the order a
configurator would: identity -> regulatory radio profile -> 'SQV?' capability
handshake -> 'SQG?' config read-back -> 'SQ?' poll-now (PortNum 256 raw-forward)
-> 'SQ>' RS485 raw bridge.

  ./test/e2e/sqc485iv2_e2e.py [--port /dev/cu.usbmodemXXXX] [--repo <path>]

Exit code 0 = every check passed. Checks that need something this bench does not
have (a wired Modbus slave) are reported SKIP, not FAIL.
"""

import argparse
import glob
import os
import struct
import subprocess
import sys
import time

import meshtastic.serial_interface
from meshtastic.protobuf import config_pb2, mesh_pb2
from pubsub import pub

PORTNUM = 256  # SILIQS_MODBUS_PORTNUM (src/modules/ModbusModule.h)

# Taiwan DTS-certified radio profile — src/mesh/Channels.cpp initDefaultLoraConfig().
# These are REGULATORY values; a mismatch is a compliance bug, not a preference.
TW_PROFILE = {
    "use_preset": False,
    "bandwidth": 500,
    "spread_factor": 9,
    "coding_rate": 5,
    "override_frequency": 922.5,
    "tx_power": 22,
    "region": "TW",
}

# src/siliqs/firmware_core/include/config.h
SQ_FEAT = {
    0x01: "TUNNEL",
    0x02: "BLE_POWER",
    0x04: "RS485_TERM",
    0x08: "POLL_NOW",
    0x10: "DEEP_SLEEP",
    0x20: "GET_CONFIG",
}
PARITY = {0: "N", 1: "E", 2: "O"}
SQ_BLOB_HDR = 37


def enum_name(descriptor, value):
    """protobuf enums come back as ints from some lib versions and strings from others."""
    return value if isinstance(value, str) else descriptor.Name(value)


class Report:
    def __init__(self):
        self.rows = []

    def _add(self, state, name, detail):
        self.rows.append((state, name, detail))
        mark = {"PASS": "  ok  ", "FAIL": " FAIL ", "SKIP": " skip ", "INFO": "  ..  "}[state]
        print(f"[{mark}] {name}" + (f" — {detail}" if detail else ""), flush=True)

    def ok(self, name, detail=""):
        self._add("PASS", name, detail)

    def fail(self, name, detail=""):
        self._add("FAIL", name, detail)

    def skip(self, name, detail=""):
        self._add("SKIP", name, detail)

    def info(self, name, detail=""):
        self._add("INFO", name, detail)

    def check(self, name, cond, detail=""):
        (self.ok if cond else self.fail)(name, detail)
        return cond

    def failures(self):
        return [r for r in self.rows if r[0] == "FAIL"]


def crc16(buf):
    """Modbus CRC16 — mirrors modbus_crc16() in firmware_core/modbus.c."""
    crc = 0xFFFF
    for byte in buf:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def source_fw_version(repo):
    """SQ_FW_VERSION as the source tree declares it, so the test never drifts."""
    path = os.path.join(repo, "src/siliqs/firmware_core/include/config.h")
    with open(path) as fh:
        for line in fh:
            if line.startswith("#define SQ_FW_VERSION"):
                return line.split('"')[1]
    return None


def source_config_version(repo):
    path = os.path.join(repo, "src/siliqs/firmware_core/include/config.h")
    with open(path) as fh:
        for line in fh:
            if line.startswith("#define SQ_CONFIG_VERSION"):
                return int(line.split()[2])
    return None


# Paths whose content decides what ends up in the image. A commit that touches
# nothing here cannot have changed the firmware, however much else it changed.
FIRMWARE_PATHS = ("src", "variants", "boards", "platformio.ini", "bin/platformio-custom.py")


def _git(repo, *args):
    """Run git, returning stdout only on success.

    The exit code matters here and is easy to drop: `git rev-parse deadbeef:src`
    echoes its own argument to stdout and exits 128, so ignoring the status turns
    an unknown commit into a plausible-looking object id.
    """
    done = subprocess.run(
        ["git", "-C", repo, *args], capture_output=True, text=True, timeout=15
    )
    return done.stdout.strip() or None if done.returncode == 0 else None


def _firmware_fingerprint(repo, commit):
    """Object ids of the firmware-relevant paths at `commit`, or None if unknown."""
    ids = []
    for path in FIRMWARE_PATHS:
        oid = _git(repo, "rev-parse", f"{commit}:{path}")
        if oid is None:
            return None  # commit not in this checkout, or the layout moved
        ids.append(oid)
    return tuple(ids)


def firmware_provenance(repo, firmware_version):
    """Does the running image correspond to this tree's FIRMWARE?

    The obvious check — does the reported git hash equal HEAD — is wrong in a way
    that matters for a release gate: it fails after a docs-only commit, and a
    check that cries wolf on a README edit is one people learn to ignore. What
    the gate actually needs to know is whether the firmware sources differ, so
    compare the tree objects of the paths that decide the image.

    Returns (ok, message).
    """
    head = _git(repo, "rev-parse", "--short=9", "HEAD")
    if not head:
        return None, "not a git checkout"
    board = firmware_version.rsplit(".", 1)[-1]  # 2.7.26.b2ffc23 -> b2ffc23
    if head.startswith(board):
        return True, f"built from HEAD ({board})"

    want, got = _firmware_fingerprint(repo, "HEAD"), _firmware_fingerprint(repo, board)
    if got is None:
        return False, (
            f"the board reports {board}, which is not a commit in this checkout — "
            "it was built somewhere else, or from a branch you do not have"
        )
    if got == want:
        moved = _git(repo, "rev-list", "--count", f"{board}..HEAD") or "?"
        return True, (
            f"board is {board}, HEAD is {head} — {moved} commit(s) apart but the "
            "firmware sources are identical, so the image matches this tree"
        )
    changed = _git(repo, "diff", "--name-only", board, "HEAD", "--", *FIRMWARE_PATHS)
    n = len(changed.splitlines()) if changed else 0
    return False, (
        f"board is {board}, HEAD is {head}, and {n} firmware file(s) differ — "
        "the board is NOT running this tree's firmware"
    )


class Node:
    """Serial link plus a PortNum-256 inbox the SQ round-trips wait on."""

    def __init__(self, port):
        self.inbox = []
        pub.subscribe(self._on_receive, "meshtastic.receive.data")
        self.iface = meshtastic.serial_interface.SerialInterface(devPath=port)
        self.num = self.iface.myInfo.my_node_num

    def _on_receive(self, packet, interface):  # noqa: ARG002 — pubsub signature
        dec = packet.get("decoded", {})
        if dec.get("portnum") in (PORTNUM, "PRIVATE_APP"):
            self.inbox.append(bytes(dec.get("payload", b"")))

    def ask(self, payload, match, timeout=10.0):
        """Send `payload` to ourselves and wait for a reply starting with `match`.

        Only packets that arrive AFTER the send count — the interface replays the
        node's stored packet log at connect time, and an old uplink sitting in that
        backlog would otherwise be mistaken for a fresh reply.
        """
        return self.request(payload, lambda pkt: pkt.startswith(match), timeout)

    def request(self, payload, want, timeout=10.0):
        seen = len(self.inbox)
        self.iface.sendData(payload, destinationId=self.num, portNum=PORTNUM, wantAck=False)
        deadline = time.time() + timeout
        while time.time() < deadline:
            for pkt in self.inbox[seen:]:
                if want(pkt):
                    return pkt
            time.sleep(0.05)
        return None

    def close(self):
        self.iface.close()


def check_identity(node, rep, repo):
    info, meta = node.iface.myInfo, node.iface.metadata
    me = node.iface.getMyNodeInfo()

    rep.info("node", f"!{node.num:08x}  {me['user']['longName']}  role={me['user'].get('role', 'CLIENT')}")
    rep.check("pio_env is the product env", info.pio_env == "sqc485iv2-esp32c3-sx1262", info.pio_env)
    hw = enum_name(mesh_pb2.HardwareModel, meta.hw_model)
    rep.check("hw_model is PRIVATE_HW", hw == "PRIVATE_HW", hw)

    fw = meta.firmware_version
    rep.info("firmware_version", fw)
    ok, why = firmware_provenance(repo, fw)
    if ok is None:
        rep.skip("the board runs this tree's firmware", why)
    else:
        rep.check("the board runs this tree's firmware", ok, why)


def check_lora(node, rep):
    lc = node.iface.localNode.localConfig.lora
    got = {
        "use_preset": lc.use_preset,
        "bandwidth": lc.bandwidth,
        "spread_factor": lc.spread_factor,
        "coding_rate": lc.coding_rate,
        "override_frequency": round(lc.override_frequency, 3),
        "tx_power": lc.tx_power,
        "region": enum_name(config_pb2.Config.LoRaConfig.RegionCode, lc.region),
    }
    rep.info("lora", " ".join(f"{k}={v}" for k, v in got.items()))
    for key, want in TW_PROFILE.items():
        rep.check(f"TW DTS profile: {key}", got[key] == want, f"got {got[key]}, want {want}")


def check_capability(node, rep, repo):
    reply = node.ask(b"SQV?", b"SQV")
    if not rep.check("SQV? capability handshake answered", reply is not None):
        return None

    proto, blob_ver, feats, fw_len = reply[3], reply[4], reply[5], reply[6]
    i = 7
    fw = reply[i:i + fw_len].decode(); i += fw_len
    pid = ""
    if proto >= 2 and i < len(reply):
        pid_len = reply[i]; i += 1
        pid = reply[i:i + pid_len].decode()
    names = "|".join(n for bit, n in SQ_FEAT.items() if feats & bit)
    rep.info("capability", f"proto={proto} blob=v{blob_ver} feat=0x{feats:02x}({names}) fw={fw} product={pid}")

    rep.check("capability proto is 2 (product_id appended)", proto == 2, str(proto))
    rep.check("product_id is SQC485Iv2", pid == "SQC485Iv2", pid)
    want_fw = source_fw_version(repo)
    rep.check("SQ_FW_VERSION matches the source tree", fw == want_fw, f"node {fw}, source {want_fw}")
    want_blob = source_config_version(repo)
    rep.check("blob version matches SQ_CONFIG_VERSION", blob_ver == want_blob, f"node {blob_ver}, source {want_blob}")
    rep.check("all six features advertised", feats == 0x3F, f"0x{feats:02x}")
    return blob_ver


def parse_blob(blob):
    """Decode a config blob (config_to_blob layout, v3/v4)."""
    ver, flags = blob[2], blob[3]
    cfg = {
        "version": ver,
        "rs485_enabled": not (flags & 0x01),
        "tunnel_enabled": bool(flags & 0x02),
        "confirmed": bool(flags & 0x04),
        "name": blob[4:20].split(b"\x00")[0].decode(errors="replace"),
        "baud": struct.unpack_from("<I", blob, 20)[0],
        "parity": PARITY.get(blob[24], "?"),
        "stop_bits": blob[25],
        "response_timeout_ms": struct.unpack_from("<H", blob, 26)[0],
        "retries": blob[28],
        "poll_gap_ms": struct.unpack_from("<H", blob, 29)[0],
        "uplink_interval_s": struct.unpack_from("<I", blob, 31)[0],
        "deep_sleep": bool(blob[35]),
        "poll_count": blob[36],
    }
    cfg["polls"] = [
        dict(zip(("slave", "function", "reg_start", "reg_count"),
                 struct.unpack_from("<BBHH", blob, SQ_BLOB_HDR + i * 6)))
        for i in range(cfg["poll_count"])
    ]
    tx = SQ_BLOB_HDR + cfg["poll_count"] * 6
    cfg["dest_node"], cfg["tx_channel"] = struct.unpack_from("<IB", blob, tx)
    if ver >= 4:
        cfg["peer_node"], cfg["idle_gap_ms"] = struct.unpack_from("<IH", blob, tx + 5)
    return cfg


def check_get_config(node, rep, blob_ver):
    reply = node.ask(b"SQG?", b"SQG")
    if not rep.check("SQG? config read-back answered", reply is not None):
        return None

    blob = reply[3:]
    rep.check("blob magic is 'SQ'", blob[:2] == b"SQ", repr(blob[:2]))
    want_crc = struct.unpack_from("<H", blob, len(blob) - 2)[0]
    rep.check("blob CRC16 verifies", crc16(blob[:-2]) == want_crc,
              f"computed 0x{crc16(blob[:-2]):04x}, blob 0x{want_crc:04x}")
    if blob_ver:
        rep.check("blob version matches the capability reply", blob[2] == blob_ver, str(blob[2]))

    cfg = parse_blob(blob)
    expect_len = SQ_BLOB_HDR + cfg["poll_count"] * 6 + 5 + (6 if cfg["version"] >= 4 else 0) + 2
    rep.check("blob length matches the declared poll_count", len(blob) == expect_len,
              f"{len(blob)} bytes, expected {expect_len}")

    rep.info("config", f"name={cfg['name']!r} rs485={cfg['rs485_enabled']} tunnel={cfg['tunnel_enabled']} "
                       f"{cfg['baud']}{cfg['parity']}{cfg['stop_bits']} timeout={cfg['response_timeout_ms']}ms "
                       f"retries={cfg['retries']} interval={cfg['uplink_interval_s']}s "
                       f"deep_sleep={cfg['deep_sleep']} dest=0x{cfg['dest_node']:08x}")
    for i, p in enumerate(cfg["polls"]):
        rep.info(f"  poll[{i}]", f"slave={p['slave']} fc={p['function']} "
                                 f"reg={p['reg_start']}+{p['reg_count']} -> {2 + 2 * p['reg_count']} bytes")
    return cfg


def decode_raw_forward(payload, cfg):
    """Slice a raw-forward payload by the poll plan (poll.c: 2 + 2*reg_count each)."""
    out, off = [], 0
    for p in cfg["polls"]:
        n = 2 + 2 * p["reg_count"]
        frame = payload[off:off + n]
        off += n
        if len(frame) < 2:
            out.append((p, frame, "TRUNCATED"))
            continue
        err = bool(frame[1] & 0x80)
        out.append((p, frame, f"ERROR(code {frame[2]})" if err and len(frame) > 2 else ("ERROR" if err else "ok")))
    return out, off


def bus_budget(cfg):
    """How long to wait for anything that has to take a turn on the RS485 bus.

    Every poll blocks the module thread for (retries + 1) x response_timeout when
    nothing answers, and a request queues behind whatever the periodic thread is
    already doing — so allow well over the nominal plan time (measured ~25 s for
    the 3-poll / 3-retry / 1000 ms default against a bare bus).
    """
    plan_s = cfg["poll_count"] * (cfg["retries"] + 1) * cfg["response_timeout_ms"] / 1000.0
    return 10 + 2.5 * plan_s


def check_poll_now(node, rep, cfg):
    if cfg is None:
        rep.skip("SQ? poll-now", "no config read-back to slice the payload with")
        return
    if not cfg["rs485_enabled"]:
        rep.skip("SQ? poll-now", "rs485_enabled=false — the node is configured not to poll")
        return

    budget = bus_budget(cfg)
    # Telemetry is raw Modbus bytes; anything starting 'SQ' is a command or a reply.
    payload = node.request(b"SQ?", lambda pkt: not pkt.startswith(b"SQ"), timeout=budget)
    if not rep.check("SQ? poll-now produced a PortNum 256 uplink", payload is not None,
                     f"waited {budget:.0f}s"):
        return

    frames, consumed = decode_raw_forward(payload, cfg)
    expect = sum(2 + 2 * p["reg_count"] for p in cfg["polls"])
    rep.check("raw-forward length matches the poll plan", len(payload) == expect,
              f"{len(payload)} bytes, plan says {expect}")
    rep.check("payload slices cleanly into per-poll frames", consumed == len(payload),
              f"consumed {consumed} of {len(payload)}")

    good = 0
    for i, (p, frame, state) in enumerate(frames):
        rep.info(f"  frame[{i}]", f"slave={p['slave']} fc={p['function']} {frame.hex()} -> {state}")
        good += state == "ok"
    if good:
        rep.ok("a Modbus slave answered", f"{good}/{len(frames)} poll(s) returned real data")
    else:
        # Error frames ARE the contract when nothing is wired — byte alignment is preserved.
        rep.skip("a Modbus slave answered", "no RS485 slave on this bench; all polls are error frames")
        rep.check("error frames keep the cloud decoder aligned",
                  all(f[1][1] & 0x80 for f in frames), "every frame should have the 0x80 exception bit")


def check_raw_bridge(node, rep, cfg):
    """'SQ>' USB->RS485 terminal: 6-byte link header + raw bytes, reply is 'SQ<'."""
    if cfg is None:
        rep.skip("SQ> RS485 raw bridge", "no config read-back")
        return
    req = bytes([1, 3, 0, 0, 0, 1])            # slave 1, FC03, reg 0, 1 register
    req += struct.pack("<H", crc16(req))
    hdr = struct.pack("<IBB", cfg["baud"], 0, cfg["stop_bits"])  # baud, parity N, stop
    reply = node.ask(b"SQ>" + hdr + req, b"SQ<", timeout=bus_budget(cfg))
    if not rep.check("SQ> raw bridge answered", reply is not None):
        return
    body = reply[3:]
    if body:
        rep.ok("raw bridge returned slave bytes", body.hex())
    else:
        rep.skip("raw bridge returned slave bytes", "empty reply — no slave answered on the bus")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial port (default: first /dev/cu.usbmodem*)")
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), help="firmware checkout to compare against")
    args = ap.parse_args()

    port = args.port or next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)
    if not port:
        print("no /dev/cu.usbmodem* found — is the node plugged in?", file=sys.stderr)
        return 2

    rep = Report()
    print(f"== SQC485Iv2 e2e — {port} ==\n")
    node = Node(port)
    try:
        print("-- identity --")
        check_identity(node, rep, args.repo)
        print("\n-- LoRa regulatory profile --")
        check_lora(node, rep)
        print("\n-- SQV? capability handshake --")
        blob_ver = check_capability(node, rep, args.repo)
        print("\n-- SQG? config read-back --")
        cfg = check_get_config(node, rep, blob_ver)
        print("\n-- SQ? poll-now (raw-forward on PortNum 256) --")
        check_poll_now(node, rep, cfg)
        print("\n-- SQ> RS485 raw bridge --")
        check_raw_bridge(node, rep, cfg)
    finally:
        node.close()

    counts = {s: sum(1 for r in rep.rows if r[0] == s) for s in ("PASS", "FAIL", "SKIP")}
    print(f"\n== {counts['PASS']} passed, {counts['FAIL']} failed, {counts['SKIP']} skipped ==")
    for _, name, detail in rep.failures():
        print(f"   FAIL {name} — {detail}")
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
