# HANDOVER.md — sqc485iv2-mesh-fw is retired

**2026-07-31.** This repo is **archived (read-only) on GitHub.** Do not plan work here —
the active repo for both Siliqs product lines is:

```
/Users/delorescelteh/Projects/sqs-sensor-mesh-fw
git@github.com:siliqs/sqs-sensor-mesh-fw.git
```

## Why this file exists

`sqs-sensor-mesh-fw`'s `CLAUDE.md` claimed (2026-07-29) that this repo had been merged
into it wholesale and could retire. **That claim was wrong** — this repo kept getting 5
more commits after the merge point (`feat/rf-03-factory-channel`: the
apply-a-shortened-interval fix, RF-03 factory-channel gate, node-identity-on-reflash
docs, the factory-PSK-is-public policy) that the other repo never picked up. The two
repos drifted — 6 shared files out of sync — inside 24 hours of the "merge" being
declared done.

That gap was closed 2026-07-30/31: the 5 stray commits were cherry-picked into
`sqs-sensor-mesh-fw`, the same interval-apply bug that `ModbusModule` had was found and
fixed in `SensorTHModule` too (this repo never had `SensorTHModule` — that's SQS-TH-I,
which only ever lived in the other repo), and this repo was set `archived` on GitHub via
`gh repo archive siliqs/sqc485iv2-mesh-fw --yes`.

**The lesson, if you're an agent reading this because someone asked you to "check the old
repo":** a merge claim in a doc is not evidence a merge happened. Diff the two trees
before trusting either one's CLAUDE.md.

## What was still uncommitted here when this repo retired

One doc-only commit was recovered and committed as part of retiring this repo — a real
finding (Full Disk Access being the actual cause of `op` hanging in Aqua sessions,
2026-07-30) that had never been committed. If you find more uncommitted work in this
tree, it did NOT make it to `sqs-sensor-mesh-fw` automatically — check before assuming
it's preserved anywhere, and if it's product-relevant (not macOS/tooling notes like the
FDA finding), port it over by hand.

## If you think you need to change something here anyway

You almost certainly don't — the QC gate (`test/hil/`), the firmware source
(`src/modules/ModbusModule.cpp`, `src/siliqs/firmware_core/`), and the docs are all
newer and more complete on the other repo. `git push` here will likely be rejected by
GitHub outright (archived repos are read-only at the git-protocol level, not just the
web UI) — if you're trying to push and it's failing, that is expected, not a bug to
work around. If there's a genuine reason to unarchive (e.g. recovering more uncommitted
local work), that's a call for the human, not something to do unprompted.
