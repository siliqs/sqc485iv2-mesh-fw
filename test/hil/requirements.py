"""
requirements.py — the QC acceptance checklist for the SQC485Iv2 and SQS-TH-I.

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
the node advertises all seven feature bits (0xBF), while three of those features
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
    "factory" the board must be running a SHIPPING image (--factory), not a CI
              build -- a CI artifact has no factory channel by construction

`product` gates a requirement on WHICH board is on the bench -- SQC485Iv2 and
SQS-TH-I share the SQ protocol and config-blob contract, but have almost
nothing else in common (one has RS485 and a tunnel, the other a built-in
sensor and no RS485 at all). sq_hil.py detects which is connected
(sq_device.detect_product) and skips every requirement whose `product`
doesn't match, the same way --skip-rs485 skips the "rs485" needs group.
    "sqc485iv2"  the default -- every requirement written before 2026-07-31
    "sqs-th-i"   SQS-TH-I only (built-in HDC1080, duty-cycle sensor leaf)
    "shared"     both -- currently just the TW DTS radio profile (RF-01/02),
                 which is identical firmware code (#if defined(SQC485IV2) ||
                 defined(SQS_TH_I)) for both products
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
    product: str = "sqc485iv2"


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
        "ID-03", "identity", "advertises all seven feature bits (0xBF)",
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
        product="shared",
    ),
    Requirement(
        "RF-02", "radio", "Bluetooth is off by default",
        "a fielded node must not advertise an open BLE reconfiguration surface "
        "out of the box, regardless of which product line it is",
        product="shared",
    ),
    Requirement(
        "RF-03", "radio", "a shipping image carries a factory channel with a private PSK",
        "release-sqc485iv2.yml builds without SQ_FACTORY_CH1_*, so the CI artifact "
        "comes out on the public default channel -- any stock Meshtastic app joins "
        "it. The only thing currently stopping someone flashing that binary as "
        "product is a sentence in the tag annotation. This also catches a PSK of an "
        "invalid AES length, which is silently accepted at build time and then "
        "produces a node that looks flashed but talks to nothing",
        needs="factory",
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
    # ── gateway Modbus-slave passive reply (config v5) ──────────────────────
    Requirement(
        "SLAVE-01", "gateway", "transparent address with an empty cache replies illegal-address (0x02)",
        "a cache-miss that goes unanswered, or that hands back something that "
        "looks like a reading, both mislead the external master",
        needs="rs485",
    ),
    Requirement(
        "SLAVE-02", "gateway", "aggregated address with an empty cache replies illegal-address (0x02)",
        "same stakes as SLAVE-01, exercised through the aggregated addressing "
        "path instead of the transparent one",
        needs="rs485",
    ),
    Requirement(
        "SLAVE-03", "gateway", "an address that is neither ours nor the aggregated one stays silent",
        "answering for an address that is not ours diverges from how a real "
        "slave behaves, and can interfere with other devices on the bus",
        needs="rs485",
    ),
    Requirement(
        "SLAVE-04", "gateway", "our address with an unsupported function code replies illegal-function (0x01)",
        "a wrong address and a wrong function code are two different field "
        "faults and must be distinguishable from the reply alone",
        needs="rs485",
    ),
    Requirement(
        "SLAVE-05", "gateway", "transparent address answers with a peer's live cached reading",
        "SLAVE-01..04 only prove the empty-cache behaviour; this is the one "
        "that proves the whole chain — a peer polls, uplinks over mesh, the "
        "gateway caches it, and an external master can read it back",
        needs="peer",
    ),
    Requirement(
        "SLAVE-06", "gateway", "aggregated address answers the same cached reading at its virtual offset",
        "aggregated addressing (virtual offset -> map row -> cache key) is a "
        "different code path from transparent addressing and has to be "
        "verified on its own",
        needs="peer",
    ),
    Requirement(
        "SLAVE-07", "gateway", "a reading older than the staleness threshold replies gateway-target-failed-to-respond (0x0B)",
        "handing an external master a stale value as if it were fresh is "
        "worse than not answering at all -- it can be mistaken for a live "
        "reading",
        needs="peer",
    ),
    # ── platform ─────────────────────────────────────────────────────────────
    Requirement(
        "BOOT-01", "boot", "the board comes back from repeated resets",
        "this hardware has no bulk decoupling on 3.3V and the RF bring-up transient "
        "sags the rail past the C3 brownout threshold; the mitigation in main.cpp is "
        "load-bearing and a boot loop is this board's historical failure mode",
        needs="reboot",
    ),
    # ── SQS-TH-I ─────────────────────────────────────────────────────────────
    # First requirements for the second product line, added 2026-07-31. Coverage
    # is intentionally partial on this first pass -- see QC_RELEASE_READINESS_
    # 2026-07-31.md for what is and is not covered yet (notably: no battery/
    # charging-flag check, since vbat/soc are still M4 placeholders in the
    # firmware itself; nothing to verify until that lands).
    Requirement(
        "SENSOR-ID-01", "sqs-th-i", "reports product id SQS-TH-I",
        "the configurator's whole auto-detect flow (hide Modbus UI, show only "
        "report interval) keys off this string",
        product="sqs-th-i",
    ),
    Requirement(
        "SENSOR-ID-02", "sqs-th-i", "reported firmware matches SQ_FW_VERSION in this checkout",
        "the single check that the board is running the build being released, and "
        "not whatever was flashed last week -- same requirement as SQC485Iv2's "
        "ID-04, restated because ID-04 itself is product='sqc485iv2' and would "
        "otherwise never run against this board",
        product="sqs-th-i",
    ),
    Requirement(
        "SENSOR-CAP-01", "sqs-th-i", "advertises the sensor feature set and nothing else",
        "SQ_FEATURES for SQS-TH-I is POLL_NOW|DEEP_SLEEP|GET_CONFIG|SENSOR (0x78) "
        "-- it must not advertise TUNNEL/BLE_POWER/RS485_TERM, none of which this "
        "product has; the configurator hides the Modbus UI based on the SENSOR "
        "bit alone, so a wrong feature set breaks that auto-detect silently",
        product="sqs-th-i",
    ),
    Requirement(
        "SENSOR-READ-01", "sqs-th-i", "the on-board HDC1080 produces a plausible reading",
        "PortNum 258 (Siliqs health) with a bench-plausible temperature/humidity "
        "range and the sensor-NACK flag clear is the difference between 'the "
        "sensor works' and 'the sensor is present and returning garbage'",
        product="sqs-th-i",
    ),
    Requirement(
        "SENSOR-READ-02", "sqs-th-i", "emits standard Meshtastic EnvironmentMetrics",
        "PortNum 67 is what makes this node's readings visible to the whole "
        "Meshtastic app/MQTT ecosystem, not just the Siliqs-specific decoder on "
        "port 258 -- losing this silently would not break anything Siliqs-side",
        product="sqs-th-i",
    ),
    Requirement(
        "SENSOR-SLEEP-01", "sqs-th-i", "duty-cycle deep sleep sleeps and wakes on schedule",
        "a power-save leaf that fails to wake is a dead node in the field, and "
        "one that fails to sleep is a flat battery -- same stakes as SQC485Iv2's "
        "FEAT-03, unexercised here until now",
        needs="role", product="sqs-th-i",
    ),
]

BY_ID = {r.id: r for r in REQUIREMENTS}
AREAS = list(dict.fromkeys(r.area for r in REQUIREMENTS))
