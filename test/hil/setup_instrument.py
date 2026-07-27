#!/usr/bin/env python3
"""
setup_instrument.py — turn a spare SQC485Iv2 into a bench instrument.

The instrument is a radio front-end, nothing more: it carries the same channel
and radio profile as the device under test so it can hear it, and it does NOT
poll RS485 of its own. Everything clever — playing the far end of a tunnel,
answering as a fake Modbus peer, injecting failures — lives in Python on the
host, so the node keeps running unmodified product firmware and there is no
test-only build to maintain.

    python3 test/hil/setup_instrument.py --port /dev/cu.usbmodemXXXX \
        [--channel-url-file dut_channel.url]

`rs485_enabled = false` is the product's own flag for this (a gateway is a node
with the flag off), so nothing here depends on a special build.
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import time

import meshtastic.serial_interface
import serial

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import sq_protocol as sq  # noqa: E402
from sq_device import Device  # noqa: E402


def open_iface(port: str, attempts: int = 6):
    """Open the API, working around the cold-handshake failure on CDC-ACM."""
    last = None
    for _attempt in range(attempts):
        try:
            with serial.Serial(port, 115200, timeout=0.5) as raw:
                raw.reset_input_buffer()
                raw.reset_output_buffer()
            time.sleep(0.5)
            iface = meshtastic.serial_interface.SerialInterface(devPath=port)
            iface.waitForConfig()
            return iface
        except Exception as exc:  # noqa: BLE001
            last = exc
            time.sleep(1.5)
    raise SystemExit(f"could not open {port}: {last}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--port", required=True, help="the instrument node's USB console")
    ap.add_argument(
        "--channel-url-file", help="file holding a Meshtastic channel URL to adopt"
    )
    ap.add_argument(
        "--name",
        default="bench-instrument",
        help="device_name written into its config blob",
    )
    args = ap.parse_args()

    # ── 1. same channel as the device under test ─────────────────────────────
    if args.channel_url_file:
        url = pathlib.Path(args.channel_url_file).read_text().strip()
        iface = open_iface(args.port)
        print("adopting the channel set from the DUT (PSK not shown) …")
        iface.localNode.setURL(url)
        time.sleep(2)
        iface.close()
        # setURL reboots the node; the port can come back under a new name.
        time.sleep(8)

    iface = open_iface(args.port)
    lora = iface.localNode.localConfig.lora
    ch = iface.localNode.channels[0]
    print(f"  channel0 : {ch.settings.name!r}  psk {len(ch.settings.psk)} bytes")
    print(
        f"  lora     : bw={lora.bandwidth} sf={lora.spread_factor} freq={lora.override_frequency} "
        f"region={lora.region} tx={lora.tx_power}"
    )
    iface.close()
    time.sleep(2)

    # ── 2. stop it polling RS485 of its own ──────────────────────────────────
    # Otherwise the instrument fills the mesh with its own error frames (nothing
    # is wired to its bus) and its telemetry is indistinguishable from the DUT's.
    idle = sq.Config(
        name=args.name,
        polls=[],
        rs485_enabled=False,
        uplink_interval_s=3600,
        deep_sleep=False,
    )
    with Device(args.port) as dev:
        status = dev.apply_config(idle.to_blob())
        if status != 0:
            print(
                f"  config rejected: {sq.CONFIG_STATUS.get(status, status)}",
                file=sys.stderr,
            )
            return 1
        back, _ = dev.get_config()
        print(
            f"  config   : name={back.name!r} rs485_enabled={back.rs485_enabled} "
            f"polls={len(back.polls)} interval={back.uplink_interval_s}s"
        )
        if back.rs485_enabled or back.polls:
            print(
                "  the instrument is still set to poll — it will pollute the mesh",
                file=sys.stderr,
            )
            return 1

    print("\ninstrument ready: it listens, it does not poll.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
