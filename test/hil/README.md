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

|                       |                                                                                                                                       |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| **identity**          | `SQV?` handshake: product id, capability proto, feature bits, and that the reported firmware matches `SQ_FW_VERSION` in this checkout |
| **radio profile**     | the _running_ LoRa config against the Taiwan DTS envelope — 922.5 MHz, BW500, SF9, CR4:5, region TW, 22 dBm                           |
| **config round trip** | writes a probe config, reads it back with `SQG?`, compares every field including the poll list                                        |
| **RS485 end to end**  | installs a poll plan, serves known registers from the dongle, triggers `SQ?`, and asserts the forwarded payload is byte-exact         |
| **error path**        | makes the slave go silent and asserts the payload is still full length, with error frames in the right slots                          |

It also records timings (config apply, poll-now round trip) — `--json` writes
them out so they can be tracked across releases.

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
