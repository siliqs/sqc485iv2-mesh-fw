# QC run records

One file per acceptance run, written by `test/hil/sq_hil.py`. Nothing here is
ever overwritten — the filename carries the moment the run started and which
node it was:

```
qc-20260728-163305-81b8a03c.md     the record
qc-20260728-163305-81b8a03c.pdf    the same thing, laid out to read/circulate

qc-<YYYYMMDD>-<HHMMSS>-<node id>
```

**The records are written in Traditional Chinese** — they are read by QC, not by
the people editing this harness. The checklist wording lives in
`../requirements_zh.py`; `../requirements.py` keeps the structure (ids, areas,
`needs`) and the codebase's English.

One line stays in English on purpose: the `實測` (observed) line under each
failure is the harness's own measurement, verbatim and identical to the terminal
output — translating it would stop the report and the log from matching. The
Chinese reading of it is the `為什麼重要` line underneath.

## Why the records exist

The terminal output answers "is this build good?" today. These files answer the
question QC is actually asked later: **what did this board do on the day we
shipped it?** So each record carries not just the verdict but everything needed
to interpret it without having been at the bench — the ports, the firmware and
`SQ_FW_VERSION` the board reported, the git branch and commit, whether the
working tree was dirty, and for anything that did not pass, the requirement's own
statement of *why it matters*.

The "Source under test" section is the part people forget. A run against a
modified working tree says so in the report, because otherwise the commit hash in
it is a lie.

## The PDF is not committed

Both files come from the same run data — `report.py` writes the Markdown and
lays the PDF out directly with `fpdf2`, rather than parsing the Markdown back.
The PDF is **not** committed: it is a presentation of a record we already keep,
and binary blobs age badly in git. The Markdown is the record.

Laying the PDF out here rather than shelling out to an external converter is
deliberate — see `report.py`'s module docstring. The short version: an absolute
path outside the repo cannot survive moving the harness to another QC machine,
and fpdf's default justified alignment mangles Chinese text that contains any
ASCII word.

## Turning it off

`--no-report` skips recording, `--report-dir DIR` writes somewhere else. Neither
is something a release run should use.

If the PDF cannot be rendered the run still records the Markdown and prints why —
a missing PDF is a note, never a QC failure.
