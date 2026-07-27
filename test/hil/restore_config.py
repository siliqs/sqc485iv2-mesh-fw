#!/usr/bin/env python3
"""
restore_config.py — put a node's configuration back.

The HIL run saves the config it found and writes it back when it finishes, but a
run that dies badly enough cannot. It prints the blob when that happens, and this
puts it back:

    python3 test/hil/restore_config.py --port /dev/cu.usbmodemXXXX --blob 5351040...

With no --blob it writes the shipped defaults instead, which is the right answer
when the original is gone and you just want a node that behaves normally again.

A node polling a bus that does not answer spends most of its time blocked inside
the poll loop, so it is quiesced first — otherwise the write is a fight with a
device that only listens between cycles.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import sq_protocol as sq  # noqa: E402
from sq_device import Device, DeviceError  # noqa: E402

# What the firmware itself starts from (firmware_core/config.c).
FACTORY = sq.Config(
    name="SQC485I",
    polls=[sq.Poll(1, 3, 0, 2), sq.Poll(1, 3, 8, 1), sq.Poll(9, 3, 0, 1)],
    baud=9600,
    response_timeout_ms=1000,
    retries=3,
    poll_gap_ms=20,
    uplink_interval_s=60,
    deep_sleep=True,
)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--port", required=True)
    ap.add_argument("--blob", help="hex config blob, as printed by a failed HIL run")
    ap.add_argument(
        "--factory", action="store_true", help="write the shipped defaults instead"
    )
    ap.add_argument(
        "--bench",
        action="store_true",
        help="leave it awake and idle: no polling, no deep sleep — for bench work",
    )
    args = ap.parse_args()

    if args.blob:
        blob = bytes.fromhex(args.blob.strip())
        sq.parse_blob(blob)  # refuse to write something the node would reject
        what = "the saved configuration"
    elif args.bench:
        blob = sq.Config(
            name="bench",
            polls=[],
            rs485_enabled=False,
            uplink_interval_s=3600,
            deep_sleep=False,
        ).to_blob()
        what = "a bench-idle configuration"
    elif args.factory:
        blob = FACTORY.to_blob()
        what = "the shipped defaults"
    else:
        print("give --blob, --factory or --bench", file=sys.stderr)
        return 2

    with Device(args.port) as dev:
        print(f"node 0x{dev.node_num:08x}")
        try:
            before = dev.quiesce()
            print(
                f"  quiesced (was: {before.name!r}, {len(before.polls)} poll(s), "
                f"rs485={before.rs485_enabled}, deep_sleep={before.deep_sleep})"
            )
        except DeviceError as exc:
            print(f"  could not quiesce ({exc}) — writing anyway", file=sys.stderr)

        status = dev.apply_config(blob)
        if status != 0:
            print(
                f"  rejected: {sq.CONFIG_STATUS.get(status, status)}", file=sys.stderr
            )
            return 1

        back, _ = dev.get_config()
        print(
            f"  wrote {what}: name={back.name!r} polls={len(back.polls)} "
            f"rs485={back.rs485_enabled} deep_sleep={back.deep_sleep} "
            f"interval={back.uplink_interval_s}s"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
