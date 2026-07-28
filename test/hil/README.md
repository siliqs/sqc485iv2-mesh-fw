# Hardware in the loop

Acceptance run against a real SQC485Iv2: the checks that a host test cannot make,
because they depend on the built image actually running on the board.

```sh
python3 -m venv test/hil/.venv
test/hil/.venv/bin/pip install -r test/hil/requirements.txt

./bin/sq-test.sh --hil -- --device /dev/cu.usbmodem844201 --rs485 /dev/cu.wchusbserial84410
```

Or call the runner directly:

```sh
test/hil/.venv/bin/python test/hil/sq_hil.py --skip-rs485      # console checks only
test/hil/.venv/bin/python test/hil/sq_hil.py --strict-radio    # verifying a factory image
```

## The rig

```text
  SQC485Iv2 ── USB ──┐
                     ├── this machine
  USB-RS485 dongle ──┘
        │
        └── A / B / GND wired to the device's RS485 terminals
```

The dongle plays the Modbus slave. On macOS a CH340 enumerates twice — as
`/dev/cu.wchusbserial*` (WCH driver) and `/dev/cu.usbserial-*` (generic); either
works, and the runner prefers the WCH node.

Port autodetection refuses to guess when there is more than one candidate. A Mac
usually has several `usbmodem` nodes, and connecting to the wrong one fails
seconds later with a protocol timeout that reads like a firmware bug.

## What it checks

`requirements.py` is the checklist — 35 numbered requirements, each with the
reason it matters. The run ends by printing every one of them with a verdict:

| area         |                                                                                                                                               |
| ------------ | --------------------------------------------------------------------------------------------------------------------------------------------- |
| **identity** | `SQV?` handshake: product id, capability proto, feature bits, and that the reported firmware matches `SQ_FW_VERSION` in this checkout          |
| **radio**    | the _running_ LoRa config against the Taiwan DTS envelope — 922.5 MHz, BW500, SF9, CR4:5, region TW, 22 dBm — and Bluetooth off by default     |
| **config**   | every field and the poll list round-trip; over-sized plans refused; malformed blobs NAK'd; v2/v3 still readable; survives a reboot; interval changes take effect |
| **bus**      | loopback splits the TX/DE path from the RX/`#RE` path; the raw bridge reaches a slave; the bridge honours its own link header                  |
| **poll**     | byte-exact forwarding, plan-exact length, one request per poll, fc4, `poll_gap_ms`, dead slave, refusing slave, corrupt reply, live baud change |
| **mode**     | `rs485_enabled=false` stops periodic polling; `tx_dest` unicasts; `confirmed` sets `want_ack`                                                  |
| **feature**  | the three advertised bits nothing used to exercise: `SQ P` BLE power, the RS485↔RS485 tunnel, duty-cycle deep sleep                            |
| **boot**     | the board comes back from repeated resets — this hardware's historical failure mode                                                            |

It also records timings (config apply, poll-now round trip) — `--json` writes
them, plus the per-requirement verdicts, so they can be tracked across releases.

## Every run is recorded

No flag needed. Each run writes a dated record to `reports/`, as Markdown and as
a PDF, and never overwrites an earlier one:

```
reports/qc-20260728-163305-81b8a03c.md
reports/qc-20260728-163305-81b8a03c.pdf
```

The records are **in Traditional Chinese** — QC reads them, and the wording lives
in `requirements_zh.py` (`requirements.py` keeps the structure and the codebase's
English). `tests/test_requirements_zh.py` fails if a requirement has no
translation, so a new check cannot quietly ship an English line into a Chinese
report.

The record carries what is needed to read it months later without having been at
the bench: the ports, the firmware and `SQ_FW_VERSION` the board reported, the
git branch and commit — and whether the working tree was dirty, because
otherwise the commit in the report is a lie. Everything that did not pass is
listed with the requirement's own statement of why it matters.

See `reports/README.md`. `--no-report` and `--report-dir` exist; a release run
should use neither.

## A requirement nothing tested is a failure, not a silence

The gate reports three distinct outcomes, and the distinction is the point:

- **SKIP** — this bench cannot answer it (no second node, `--skip-rs485`, …). The
  reason is recorded.
- **NOT RUN** — nobody wrote a check for it. This **fails the run**.
- **FAIL** — it was tested and it did not pass.

Making `NOT RUN` fatal is deliberate. The harness used to assert that the node
advertises all six feature bits (`0x3F`) while three of those features — the
tunnel, the BLE power command, and deep sleep — had no check at all: it was
confirming the _advertisement_ and never the _feature_. Coverage should only ever
be dropped on purpose (`--allow-gaps`), never by forgetting.

When you add a feature, add its requirement to `requirements.py` first. It
becomes `NOT RUN` immediately, and the gate stays red until a check claims it.

## It puts the board back

The device's configuration is read before the run and written back in a `finally`
block, so a HIL run leaves the board as it found it. If the process is killed
hard enough to skip that, the saved blob is printed at the top of the run and can
be restored by hand.

## The radio profile is a warning, not a failure

By default a deviation from the certified profile is reported as a warning: these
are factory _defaults_, and a user is allowed to change them from the app. Pass
`--strict-radio` when what you are verifying is a factory image, where a
deviation is a real defect.

The same applies to Bluetooth: the product's factory default is off, but a board
that has been configured by hand may legitimately have it on.

## The `SQ>` link header is mandatory

⚠ A raw-bridge request is `SQ>` + **six bytes of link header** + the frame:

```text
baud (u32 LE) | parity (u8) | stop_bits (u8) | frame…
```

The header is positional and untagged, and `rawBridge()` decides it is present by
length alone: six bytes or more means header. So an 8-byte Modbus read request —
the most obvious thing to hand a raw bridge — is not rejected, it is _reinterpreted_:
`01 03 00 00 00 02 C4 0B` becomes baud `0x00000301` (769), parity 0, stop 2, and a
two-byte frame of `C4 0B`. The device then transmits garbage at 769 baud **and
leaves its UART there**, so every later poll times out too.

The symptom is a completely silent bus that looks exactly like mis-wiring. Always
build requests with `sq_protocol.bridge_request()`; the parsing rule is pinned
host-side by `test_sqcmd_bare_modbus_frame_is_misread_as_a_header`.

## Loopback runs before Modbus

`check_rs485_echo` puts the dongle in dumb-echo mode and sends a byte pattern
through the bridge. No addressing, no function codes, no CRC — so when it fails
the suspect list is short, and it splits the failure in half:

- the dongle heard the pattern → the device's **TX** path and DE (GPIO9, polarity
  per board rev) work
- the device read it back → the device's **RX** path and #RE (GPIO2, active low)
  work

Only if both pass does the Modbus section run, so a bus fault is never reported
twice in different vocabulary.

## When the bus fails, it diagnoses instead of guessing

A silent bus and a mis-framed bus fail identically at the Modbus layer. When the
raw bridge gets no answer, the runner makes the device transmit while listening
to the dongle at the raw serial level, and reports what was physically on the
wire:

- **nothing heard** — no electrical path, or the driver is never enabled; check
  A/B are landed, the transceiver has power, and DE polarity matches the board rev
- **a run of `00` or `FF`** — the line is driven but cannot be framed: the two
  ends are at different line rates (including the 769-baud trap above), A/B are
  swapped, or there is no common ground
- **the request, cleanly** — wiring is fine; the slave did not answer
- **garbled bytes** — both ends disagree on baud or framing

## Independence

`modbus_slave.py` is a hand-rolled RTU slave rather than pymodbus, on purpose: if
both ends of the test came from the same library, a shared misreading of the spec
would pass. The config blob codec in `sq_protocol.py` is shared with the unit-test
fixtures so there is exactly one host-side implementation of the wire format —
and that one is independent of the firmware's.
