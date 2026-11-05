#!/opt/homebrew/bin/python3
"""End-to-end bench test for a live SQS-TH-I node over its USB console.

Documented in CLAUDE.md §5. SQS-TH-I duty-cycle sleeps between reports (see
CLAUDE.md §3.1), so native USB CDC is only present for a few seconds per cycle
unless something holds the port open — this script does exactly that: it polls
for the port, connects the moment it appears, and holds the connection for the
whole watch window (clientConnected() sees the open port and defers sleep, so
the node just keeps reporting on schedule instead of dropping into deep sleep
mid-test).

Exercises: identity -> regulatory radio profile -> 'SQV?' capability handshake
-> 'SQG?' config read-back -> watch the mesh for at least one PortNum 258
(Siliqs health) and one PortNum 67 (EnvironmentMetrics) reading.

  ./test/e2e/sqs_th_i_e2e.py [--port /dev/cu.usbmodemXXXX] [--watch-s 100]

Exit code 0 = every check passed. This is a READ-ONLY smoke test — it never
writes config. Nothing here requires RS485 or a second node.
"""

import argparse
import glob
import struct
import sys
import time

import meshtastic.serial_interface
from meshtastic.protobuf import config_pb2
from pubsub import pub

PORTNUM_258 = 258
PORTNUM_67_NAME = "TELEMETRY_APP"

TW_PROFILE = {
    "use_preset": False,
    "bandwidth": 500,
    "spread_factor": 9,
    "coding_rate": 5,
    "override_frequency": 922.5,
    "tx_power": 22,
    "region": "TW",
}

SQ_FEAT = {
    0x08: "POLL_NOW",
    0x10: "DEEP_SLEEP",
    0x20: "GET_CONFIG",
    0x40: "SENSOR",
}


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


def enum_name(descriptor, value):
    return value if isinstance(value, str) else descriptor.Name(value)


def grab_window(port_glob, timeout_s):
    """Poll for the port and connect the instant it appears (CLAUDE.md §3.1)."""
    deadline = time.time() + timeout_s
    hits = []
    while time.time() < deadline:
        hits = sorted(glob.glob(port_glob))
        if hits:
            break
        time.sleep(0.03)
    return hits[0] if hits else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="explicit port; default polls for /dev/cu.usbmodem*")
    ap.add_argument("--watch-s", type=int, default=100, help="how long to watch for port 258/67 (default 100)")
    args = ap.parse_args()

    rep = Report()
    # Always POLL for the target path, even when --port is explicit: SQS-TH-I is
    # asleep most of the time (CLAUDE.md §3.1), so an explicit path can still not
    # exist at the moment we look.
    port = grab_window(args.port or "/dev/cu.usbmodem*", timeout_s=90)
    if not port:
        print("no SQS-TH-I port appeared within 90s", file=sys.stderr)
        return 2

    print(f"== SQS-TH-I e2e — {port} ==\n")
    iface = meshtastic.serial_interface.SerialInterface(devPath=port)
    try:
        time.sleep(1.0)  # let identity/config settle after connect

        print("-- identity --")
        node = iface.getMyNodeInfo()
        user = node.get("user", {})
        rep.info("node", f"!{node.get('num', 0):08x}  {user.get('longName', '?')}")
        hw = user.get("hwModel", "?")
        rep.check("hw_model is PRIVATE_HW", hw == "PRIVATE_HW", str(hw))
        fw = getattr(iface.metadata, "firmware_version", "?")
        rep.info("firmware_version", fw)
        role = iface.localNode.localConfig.device.role
        rep.info("role", enum_name(config_pb2.Config.DeviceConfig.Role, role))

        print("\n-- LoRa regulatory profile --")
        lora = iface.localNode.localConfig.lora
        got = {
            "use_preset": lora.use_preset,
            "bandwidth": lora.bandwidth,
            "spread_factor": lora.spread_factor,
            "coding_rate": lora.coding_rate,
            "override_frequency": round(lora.override_frequency, 1),
            "tx_power": lora.tx_power,
            "region": enum_name(config_pb2.Config.LoRaConfig.RegionCode, lora.region),
        }
        for k, want in TW_PROFILE.items():
            rep.check(f"TW DTS profile: {k}", got[k] == want, f"got {got[k]}, want {want}")

        print("\n-- SQV? capability handshake --")
        watermark = len(iface._nodesByNum) if hasattr(iface, "_nodesByNum") else 0
        seen = {}

        def on_receive(packet=None, interface=None):
            decoded = packet.get("decoded", {})
            portnum = decoded.get("portnum")
            raw = decoded.get("payload")
            if raw is None:
                return
            # SQV?/SQG? replies AND the periodic 11-byte health packet all share
            # PortNum 258 (SILIQS_SENSOR_PORTNUM) -- content, not portnum, is what
            # tells them apart. Check the ASCII command-reply signature FIRST, or
            # an 'SQV'/'SQG' reply gets misfiled as a garbage sensor reading (its
            # own bytes decode as ver=83 'S', model=81 'Q' -- that exact failure
            # mode is why this comment exists).
            if raw[:3] == b"SQV":
                seen["SQV"] = raw
            elif raw[:3] == b"SQG":
                seen["SQG"] = raw
            elif portnum == PORTNUM_67_NAME or portnum == 67:
                seen.setdefault(67, []).append((time.time(), raw))
            elif isinstance(portnum, int) and portnum == PORTNUM_258:
                seen.setdefault(258, []).append((time.time(), raw))

        pub.subscribe(on_receive, "meshtastic.receive")

        node_num = iface.myInfo.my_node_num
        iface.sendData(b"SQV?", destinationId=node_num, portNum=PORTNUM_258, wantAck=False)
        deadline = time.time() + 5
        while time.time() < deadline and "SQV" not in seen:
            time.sleep(0.1)
        cap = seen.get("SQV")
        if rep.check("SQV? capability handshake answered", cap is not None):
            proto, blob_ver, feat = cap[3], cap[4], cap[5]
            fw_len = cap[6]
            fw_str = cap[7 : 7 + fw_len].decode("ascii", "replace")
            pid_len = cap[7 + fw_len]
            pid = cap[8 + fw_len : 8 + fw_len + pid_len].decode("ascii", "replace")
            bits = "|".join(name for bit, name in SQ_FEAT.items() if feat & bit)
            rep.info("capability", f"proto={proto} blob=v{blob_ver} feat=0x{feat:02x}({bits}) fw={fw_str} product={pid}")
            rep.check("capability proto is 2 (product_id appended)", proto == 2, str(proto))
            rep.check("product_id is SQS-TH-I", pid == "SQS-TH-I", pid)
            rep.check("SENSOR feature bit is set", bool(feat & 0x40), f"0x{feat:02x}")
            rep.check(
                "RS485/tunnel/BLE-power features are NOT advertised",
                not (feat & (0x01 | 0x02 | 0x04)),
                f"0x{feat:02x}",
            )

        print("\n-- SQG? config read-back --")
        iface.sendData(b"SQG?", destinationId=node_num, portNum=PORTNUM_258, wantAck=False)
        deadline = time.time() + 5
        while time.time() < deadline and "SQG" not in seen:
            time.sleep(0.1)
        if rep.check("SQG? config read-back answered", "SQG" in seen):
            blob = seen["SQG"][3:]
            rep.check("blob magic is 'SQ'", blob[:2] == b"SQ", str(blob[:2]))
            # Layout (config.h): 'S','Q',ver,flags(4) name[16](4..19) baud(20..23)
            # parity(24) stop(25) resp_timeout(26..27) retries(28) poll_gap(29..30)
            # interval_s(31..34) deep_sleep(35) poll_count(36).
            if len(blob) >= 36:
                interval = struct.unpack("<I", blob[31:35])[0]
                deep_sleep = blob[35]
                rep.info("config", f"interval={interval}s deep_sleep={bool(deep_sleep)}")

        print(f"\n-- watching for PortNum 258 + PortNum 67 ({args.watch_s}s) --")
        deadline = time.time() + args.watch_s
        while time.time() < deadline and (67 not in seen or 258 not in seen):
            time.sleep(0.5)

        if rep.check("saw at least one PortNum 258 (Siliqs health) reading", 258 in seen):
            _, raw = seen[258][-1]
            if len(raw) >= 11:
                ver, model, temp, rh, vbat, soc, flags, boot = struct.unpack("<BBhHHBBB", raw[:11])
                rep.info(
                    "port 258",
                    f"ver={ver} model={model} T={temp/100:.2f}C RH={rh/100:.2f}% "
                    f"vbat={vbat} soc=0x{soc:02x} flags=0x{flags:02x} boot={boot}",
                )
                rep.check("payload version is 1", ver == 1, str(ver))
                rep.check("model is SQS-TH-I (1)", model == 1, str(model))
                rep.check("sensor NACK bit is clear", not (flags & 0x01), f"0x{flags:02x}")

        if rep.check("saw at least one PortNum 67 (EnvironmentMetrics) reading", 67 in seen):
            rep.info("port 67", f"{len(seen[67])} reading(s) in the watch window")

    finally:
        iface.close()

    n_fail = len(rep.failures())
    n_pass = sum(1 for r in rep.rows if r[0] == "PASS")
    n_skip = sum(1 for r in rep.rows if r[0] == "SKIP")
    print(f"\n== {n_pass} passed, {n_fail} failed, {n_skip} skipped ==")
    if n_fail:
        for _, name, detail in rep.failures():
            print(f"   FAIL {name}" + (f" — {detail}" if detail else ""))
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
