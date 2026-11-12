# GAP-004, cycle 5: the fork row, and the same repair extended

**Date:** 2026-11-12. **Tree:** `54c4324` plus the changes recorded below.

## The row

A copied folder of a project in the store is a copy (`STORE_PROBLEM_COPY`,
paused) until `store_fork` makes it a project of its own: it opens a journal
(`fork.json`, naming the source), exports the store's data for the source into
`.scribodo/fork-snapshot`, signs the export, imports the staging as a new
project through the same `import_locked_from` as a legacy import (witness,
one transaction, marker, set-aside), and then closes the journal
(`fork_snapshot_finish`: the staging's `.scribodo` into a stamped archive,
the staging removed, witness and journal removed). Three fork points
(`fork.journal_opened`, `fork.exported`, `fork.imported`, the last after the
marker and before the close) plus the import row's points on this path.

The driver imports a legacy project, copies its folder, kills a child running
`store_fork` on the copy, and as the next process does what the app does
with what it finds: a copy is forked again; a project in the store is opened
(`store_finish_import`). The invariant: a copy is paused until its marker is
replaced and a project in the store afterwards, never a plain file store; the
copy's one chat (under a rebased id, since the original owns the old one) has
its two messages; the original's chat and id are untouched; and the journal
is closed.

## Measurement, before the repair

7 deaths. Authority and data held at every one. At the three deaths after
the marker (`import.marked`, `import.set_aside`, `fork.imported`) the next
open saw a project in the store and left the staging, the witness and
`fork.json` in place: the code finished an open journal only when the folder
was forked again (a copy of a copy). The same class as the import row's
finding: an incomplete transition that a plain open never completed.

## The repair

`store_finish_import` now reads the fork journal: when it names the marker's
project as its target, the journal is closed with `fork_snapshot_finish`,
exactly as the next fork of a copy would have; a journal naming another
target is left alone. The plain-import path is unchanged.

## Measurement, after the repair

| Point | Mode at death | Staging / witness / journal at death | Next process | Journal |
|---|---|---|---|---|
| `fork.journal_opened` | copy (paused) | no / no / yes | fork again: new project | closed |
| `fork.exported` | copy | yes / no / yes | fork again | closed |
| `import.registered` | copy | yes / yes / yes | fork again, resumed under the witness | closed |
| `import.committed` | copy | yes / yes / yes | fork again | closed |
| `import.marked` | db (new project) | yes / yes / yes | open: finished here | closed |
| `import.set_aside` | db | yes / yes / yes | open: finished here | closed |
| `fork.imported` | db | yes / yes / yes | open: finished here | closed |

At every death the copy's chat had two messages once and the original's
chat and project id were unchanged. With the earlier rows: 120 deaths, 1,370
checks, 0 failures, 49 s.

One correction to the driver during the run: it first looked the copy's chat
up by the original's id; a forked copy rebases a chat id the original owns,
so the driver lists the copy's chats and loads the one it finds.

## Gates

`make test` 95,885 C checks and 161 Python tests, 0 failures; `make dbtest`
18,000, 0; `make analyze` 0 warnings across 88 source files; `make sanitize`
95,885, 0, 0 diagnostics; `make tsan` 48,549, 0, 0 diagnostics; `make crashtest`
1,370 checks, 0 failures. Every exit status 0 (`build/crash/gates5-*.log`,
`build/crash/crashtest.log`).
