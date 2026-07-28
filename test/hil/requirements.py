"""
requirements.py — the QC acceptance checklist for the SQC485Iv2.

This file is the *list*, not the tests. It enumerates everything a release build
has to be shown to do, so the run can end with a per-requirement verdict instead
of a count of assertions that happened to fire.

Why a manifest and not just "however many checks are in sq_hil.py": a gate that
only reports what it ran cannot tell you what it forgot. The most expensive kind
of release bug is the feature nobody thought to test — the harness happily prints
PASSED while a whole capability goes unexercised. Here, a requirement with no
check attached comes out as NOT RUN and fails the gate, so coverage can only be
dropped on purpose (--allow-gaps), never by accident.

The canonical example, and the reason this exists: the identity section asserts
the node advertises all six feature bits (0x3F), while three of those features
— the RS485↔RS485 tunnel, the BLE TX power command, and the duty-cycle deep
sleep — had no check at all. The gate was confirming the *advertisement* and
never the *feature*.

Each requirement records why it matters, because "POLL-06 failed" is not
actionable at 2am and "a short payload desynchronises the cloud decode for every
following poll" is.

`needs` gates a requirement on what the bench actually has:
    ""        always runnable, console only
    "rs485"   needs the USB-RS485 dongle wired to the device's A/B terminals
    "reboot"  power-cycles or resets the board (slow, and it must come back)
    "peer"    needs a SECOND Meshtastic node on USB (--peer)
    "role"    changes the Meshtastic device role (restored afterwards)
"""

from __future__ import annotations

from dataclasses import dataclass

NOT_RUN = "NOT RUN"
PASS = "PASS"
FAIL = "FAIL"
SKIP = "SKIP"


@dataclass(frozen=True)
class Requirement:
    id: str
    area: str
    title: str
    why: str
    needs: str = ""


REQUIREMENTS: list[Requirement] = [
    # ── identity / provenance ────────────────────────────────────────────────
    Requirement(
        "ID-01", "identity", "reports product id SQC485Iv2",
        "the configurator labels every node from this string; a wrong id ships a "
        "device the tooling cannot categorise",
    ),
    Requirement(
        "ID-02", "identity", "capability handshake is proto 2",
        "proto 2 is what appends product_id after the firmware string — a proto-1 "
        "reply leaves the configurator guessing which product it is talking to",
    ),
    Requirement(
        "ID-03", "identity", "advertises all six feature bits (0x3F)",
        "the configurator gates its UI on these bits; advertising a feature the "
        "build does not have is worse than not advertising it",
    ),
    Requirement(
        "ID-04", "identity", "reported firmware matches SQ_FW_VERSION in this checkout",
        "the single check that the board is running the build being released, and "
        "not whatever was flashed last week",
    ),
    # ── radio / regulatory ───────────────────────────────────────────────────
    Requirement(
        "RF-01", "radio", "running LoRa config is inside the TW DTS envelope",
        "922.5 MHz / BW500 / SF9 / CR4:5 / TW / 22 dBm is the type-approved "
        "profile; shipping outside it is a regulatory defect, not a preference",
    ),
    Requirement(
        "RF-02", "radio", "Bluetooth is off by default",
        "a fielded RS485 gateway must not advertise an open BLE reconfiguration "
        "surface out of the box",
    ),
    # ── config blob contract ─────────────────────────────────────────────────
    Requirement(
        "CFG-01", "config", "every config field survives write → read-back",
        "the configurator shows the user what it believes the node is set to; a "
        "field that silently fails to stick makes that display a lie",
    ),
    Requirement(
        "CFG-02", "config", "the poll list survives write → read-back",
        "the poll plan is also the cloud's decode key — if the node's plan and the "
        "cloud's plan differ, every value is misattributed",
    ),
    Requirement(
        "CFG-03", "config", "an over-sized poll plan is refused, not truncated",
        "8 polls x 16 registers is 272 bytes against a 233-byte packet; the engine "
        "drops whole polls off the end, so those datapoints go permanently missing "
        "with nothing anywhere saying so",
    ),
    Requirement(
        "CFG-04", "config", "a malformed blob is rejected with status 1",
        "bad magic, bad CRC, a truncated write or an unknown version must all NAK; "
        "a half-applied config is how a node ends up polling a plan nobody chose",
    ),
    Requirement(
        "CFG-05", "config", "legacy blob versions (v2, v3) are still accepted",
        "config.h promises v2/v3 remain readable — configurators in the field are "
        "not upgraded in lockstep with firmware",
    ),
    Requirement(
        "CFG-06", "config", "the configuration survives a reboot",
        "the blob lives in LittleFS; a partition or filesystem change can wipe it "
        "while every in-session check still passes",
        needs="reboot",
    ),
    Requirement(
        "CFG-07", "config", "a shortened uplink interval takes effect promptly",
        "applyConfigBlob() re-inits the UART but never reschedules the poll "
        "thread, so a node that has just returned uplink_interval_s * 1000 sleeps "
        "out the OLD interval first. Moving a node from hourly to minutely "
        "reporting then looks dead for up to an hour",
        needs="rs485",
    ),
    # ── the RS485 bus itself ─────────────────────────────────────────────────
    Requirement(
        "BUS-01", "bus", "the device drives the bus (TX path and DE)",
        "isolates a dead transceiver or a DE polarity mismatch (v231 vs v233) from "
        "a Modbus-level fault, which otherwise look identical",
        needs="rs485",
    ),
    Requirement(
        "BUS-02", "bus", "the device reads the bus (RX path and /RE)",
        "the other half of the same split: a node that transmits but never hears "
        "the reply reports every slave as absent",
        needs="rs485",
    ),
    Requirement(
        "BUS-03", "bus", "the raw bridge reaches a slave and returns its bytes",
        "the 'SQ>' terminal is how an installer proves the wiring on site before "
        "trusting any poll result",
        needs="rs485",
    ),
    Requirement(
        "BUS-04", "bus", "link parameters in the bridge request are honoured",
        "the bridge carries its own baud/parity so it works before the poll config "
        "is set; if it silently used the stored config instead, on-site diagnosis "
        "of a 19200 device would be impossible",
        needs="rs485",
    ),
    # ── the poll engine / raw-forward contract ───────────────────────────────
    Requirement(
        "POLL-01", "poll", "the raw-forward payload is byte-exact",
        "the product's whole value is that the cloud sees the slave's own bytes "
        "unmodified; any transformation here is a silent data-corruption bug",
        needs="rs485",
    ),
    Requirement(
        "POLL-02", "poll", "payload length matches the plan exactly",
        "the cloud slices by config alone (2 + 2*reg_count per poll) — a length "
        "that disagrees with the plan misaligns every following value",
        needs="rs485",
    ),
    Requirement(
        "POLL-03", "poll", "each poll produces exactly one bus request",
        "a duplicated request wastes bus time and, on a duty-cycled node, wake time",
        needs="rs485",
    ),
    Requirement(
        "POLL-04", "poll", "function code 4 (input registers) works on the wire",
        "half the field devices only expose input registers; fc4 is round-tripped "
        "in config but was never actually put on the bus by any check",
        needs="rs485",
    ),
    Requirement(
        "POLL-05", "poll", "poll_gap_ms is honoured between consecutive polls",
        "the settle delay is what keeps a slow transceiver from hearing the next "
        "request as a tail of the previous reply",
        needs="rs485",
    ),
    Requirement(
        "POLL-06", "poll", "a dead slave still yields a full-length aligned error frame",
        "dropping the poll instead would shift every following byte and corrupt the "
        "cloud decode for the whole payload, not just the failed point",
        needs="rs485",
    ),
    Requirement(
        "POLL-07", "poll", "a refusing slave is reported as an exception, not a timeout",
        "a wrong register in the plan would otherwise look exactly like an "
        "unplugged cable, sending the technician to the wrong end of the site",
        needs="rs485",
    ),
    Requirement(
        "POLL-08", "poll", "the slave's own exception code reaches the payload",
        "'illegal data address' and 'device busy' need opposite responses; without "
        "the code the technician cannot tell fix-the-plan from check-the-device",
        needs="rs485",
    ),
    Requirement(
        "POLL-09", "poll", "a permanent refusal is not retried",
        "asking three more times whether a register exists cannot change the "
        "answer; it just spends bus time and battery",
        needs="rs485",
    ),
    Requirement(
        "POLL-10", "poll", "a corrupt reply is rejected, not forwarded as data",
        "a CRC failure that reaches the cloud as a reading is worse than no "
        "reading at all — nothing downstream can tell it is garbage",
        needs="rs485",
    ),
    Requirement(
        "POLL-11", "poll", "a changed baud takes effect without a reboot",
        "applyConfigBlob re-inits the UART on purpose; if it did not, every "
        "on-site baud correction would need a power cycle to take",
        needs="rs485",
    ),
    # ── operating modes ──────────────────────────────────────────────────────
    Requirement(
        "MODE-01", "mode", "rs485_enabled=false stops all bus traffic",
        "this is the gateway SKU's mode — a 'gateway' that keeps polling a bus "
        "with nothing on it wakes needlessly and pollutes the log with errors",
        needs="rs485",
    ),
    Requirement(
        "MODE-02", "mode", "tx.dest_node sends the uplink unicast, not broadcast",
        "a deployment that routes telemetry to one collector must not be flooding "
        "the whole mesh with every reading",
    ),
    Requirement(
        "MODE-03", "mode", "tx.confirmed sets want_ack on the uplink",
        "Level-3 confirmed delivery is the difference between a reading that is "
        "known-delivered and one that was merely shouted",
    ),
    # ── advertised features with no other coverage ───────────────────────────
    Requirement(
        "FEAT-01", "feature", "'SQ P' sets BLE TX power and persists it",
        "advertised as feature bit 0x02 and never exercised by any check; it "
        "writes its own store key, so it can fail independently of the config blob",
    ),
    Requirement(
        "FEAT-02", "feature", "the RS485↔RS485 tunnel forwards a frame and writes the reply back",
        "advertised as feature bit 0x01 and never exercised; the whole tunnel SKU "
        "rests on it and it has no host-testable end",
        needs="peer",
    ),
    Requirement(
        "FEAT-03", "feature", "duty-cycle deep sleep sleeps and wakes on schedule",
        "advertised as feature bit 0x10 and never exercised; a leaf that fails to "
        "wake is a dead node in the field, and one that fails to sleep is a flat "
        "battery",
        needs="role",
    ),
    # ── platform ─────────────────────────────────────────────────────────────
    Requirement(
        "BOOT-01", "boot", "the board comes back from repeated resets",
        "this hardware has no bulk decoupling on 3.3V and the RF bring-up transient "
        "sags the rail past the C3 brownout threshold; the mitigation in main.cpp is "
        "load-bearing and a boot loop is this board's historical failure mode",
        needs="reboot",
    ),
]

BY_ID = {r.id: r for r in REQUIREMENTS}
AREAS = list(dict.fromkeys(r.area for r in REQUIREMENTS))
