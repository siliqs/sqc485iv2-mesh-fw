# firmware_core host tests

Regression tests for the portable Siliqs engine in
[`src/siliqs/firmware_core/`](../../src/siliqs/firmware_core/) — `config.c`,
`modbus.c`, `poll.c`, `sqcmd.c` — run on the development machine with a mock HAL.

```sh
make test        # build + run, with address/UB sanitizers   (~0.2 s)
make test SAN=0  # without sanitizers
make golden      # regenerate the blob fixtures
```

No PlatformIO, no Docker, no portduino, no packages. A C11 compiler is the whole
dependency list, which is what makes it realistic to run this on every edit.

Prefer `../../bin/sq-test.sh` as the entry point — it runs this suite plus the
firmware build regression.

## What this suite is protecting

The engine is small, but three of its properties are expensive to get wrong in
the field, and none of them fail loudly:

**The config blob is a versioned public contract.** A configurator in a
customer's hands still speaks v2. Every fixture in [`golden/`](golden/) was
produced by [`golden/make_golden.py`](golden/make_golden.py) — an independent
implementation of the format written from the spec in `config.h` — so the tests
compare the firmware against the documented wire format rather than against
itself. A blob fixture must never be regenerated from firmware output; that
would let an encoder bug quietly become the expected answer.

**`sizeof(sq_config_t)` is an ABI.** `config_load()` only accepts a stored record
of exactly that size, so adding one field silently factory-resets every device in
the field on its next boot. `test_config_struct_abi_is_frozen` pins the number
(500 bytes). Changing it is allowed — it just has to be a decision, made in the
same commit as a migration, rather than a side effect.

**Payload byte alignment is the cloud decoder's only contract.** The cloud slices
the concatenated raw-forward payload using nothing but the poll plan, so a failed
read has to occupy exactly as many bytes as a successful one. `poll.c` emits a
fixed-length error frame for this reason, and `test_poll_mixed_results_stay_aligned`
is what keeps it that way.

**A node must never answer its own reply.** Commands, config blobs and replies
all share the two-byte `SQ` prefix on one portnum, so telling them apart is pure
byte inspection — and getting it wrong produces a packet loop on the air rather
than a local error. `sqcmd.c` holds that decision (the Meshtastic module keeps
only what needs node state), and `test_sqcmd_own_replies_are_never_commands`
feeds every reply the firmware can emit back into the classifier.

## Layout

| Path        |                                                                  |
| ----------- | ---------------------------------------------------------------- |
| `mock/`     | HAL mocks: scriptable RS485 line, virtual clock, in-memory store |
| `golden/`   | blob fixtures + the independent generator that produces them     |
| `test_*.c`  | one file per engine unit                                         |
| `tests.def` | the registry — a test not listed here does not run               |

### The mocks are not the `14_` stubs

[`14_SQC485Iv2_basic/firmware_core/stub/`](../../../14_SQC485Iv2_basic/firmware_core/stub/)
simulates a _working_ meter so the demo runs end to end. These mocks go further
in three ways that unit tests need:

- **The RS485 line is scriptable per attempt.** A real slave will not politely
  produce a CRC error on request, so without this the retry, CRC, byte-count and
  mismatch branches in `modbus.c` are simply unreachable from a host test.
- **Time is virtual.** `hal_delay()` advances a counter instead of sleeping, so
  "the poll gap was honoured" and "a dead bus costs at most `retries+1` timeouts"
  are assertions rather than stopwatch readings — and the suite still finishes in
  a fifth of a second.
- **Every HAL call is logged.** `mock_serial_log()` returns a string like
  `+WF-R`, which is how `test_modbus_de_turnaround_order` checks that DE is
  released before the engine starts listening.

## Behaviour these tests pinned, and then changed

The suite was written against the code as it stood, including the parts that were
wrong. Writing the awkward behaviour down first is what made it safe to change:

- **A Modbus exception used to be reported as a timeout.** `transact()` waited for
  the length a _successful_ reply would have, so the 5-byte exception frame never
  completed. In the field that made "wrong register in the poll plan" identical to
  "cable unplugged". `transact()` now reads the function byte first and sizes the
  rest of the frame from it — see `test_modbus_reports_exception_replies`.

- **A dead bus cost twice what it looked like.** The mock originally returned
  whatever bytes were available; the real HAL spins until it fills the buffer or
  the timeout expires. Making the mock faithful revealed that each attempt burned
  two full timeouts, not one. Reading the reply in stages removed the second one,
  halving the worst case — see `test_modbus_timeout_budget_is_bounded`.

- **An over-sized poll plan was accepted and silently truncated.** The format can
  express 272 bytes against a 233-byte packet, and `poll_collect_raw()` drops
  whole polls off the end. The truncation itself is correct — it lands on a poll
  boundary, so the cloud decode of what arrives stays aligned — but nothing said
  it happened. The device now refuses such a plan with ack status 3. The
  truncation behaviour is still pinned by
  `test_poll_drops_polls_that_exceed_the_mesh_payload`, because it remains the
  backstop.

One limitation is still pinned as-is: an exception reply consumes the full retry
budget (`test_modbus_exception_still_consumes_retries`). The slave answered, so
retrying will not help, but the engine cannot tell a wrong register map from a
transiently busy device.

## Adding a test

1. Write it in the relevant `test_*.c` (or add a new file to `SRC` in the Makefile).
2. Register it in [`tests.def`](tests.def).
3. `make test`.

Assertions available in [`sq_test.h`](sq_test.h): `ASSERT_TRUE` / `ASSERT_FALSE` /
`ASSERT_EQ` / `ASSERT_STR_EQ` / `ASSERT_MEM_EQ` (hex-dumps both sides on failure) /
`ASSERT_FLOAT_EQ`.

Call `mock_serial_reset()`, `mock_time_reset()` and `mock_board_reset()` at the
top of any test that touches the bus — the mocks hold state deliberately so a
test can inspect it afterwards.
