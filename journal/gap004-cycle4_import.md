# GAP-004, cycle 4: the legacy import row, and a repair

**Date:** 2026-11-12. **Tree:** `584cd87` plus the changes recorded below.

## The row

`store_import` moves a file-store project into the database: it registers the
project row and writes the `import-pending` witness, imports everything in one
transaction, writes the `store.json` marker (temp, fsync, rename, directory
fsync), sets the eight legacy items aside into `.scribodo/legacy-<stamp>/`,
and removes the witness. Five points: `import.registered`, `import.committed`,
`import.marked`, `import.aside_item` after each item's rename, and
`import.set_aside`. The driver builds a legacy project (one document, a chat
of two messages, a note with two versions, a canon record) with a store
manager on a fresh throwaway database, kills a child running `store_import`,
and then, as the next process, starts a new manager, reads the mode, runs
`store_import` again and reads the data back.

The invariant: the mode at death is `STORE_FILES` without a marker and
`STORE_DB` with one, never paused; the second import succeeds; the chat has
its two messages, the note its two versions, the canon its one record, each
once; every legacy item is in a stamped archive and the witness is gone.

## Measurement, before the repair

12 deaths. Authority and data held at every one: without the marker the
files were authoritative and the second import resumed under the witness's
project id with nothing duplicated; with the marker the store was
authoritative. The second half of the invariant failed at every death after
the marker (`import.marked`, the eight `import.aside_item` hits,
`import.set_aside`: 10 of 12): the legacy files were still live under
`.scribodo/` (none, some or all of them set aside) and the witness was still
there, and the second `store_import` returned at once because the mode was
already `STORE_DB`. In the app the same holds: `store_adopt_project` asks
`store_needs_import`, which is false once the marker exists, so a move that
died after its marker was never finished, on any later open. Not data loss
(the store held everything once) and not an authority error; an incomplete
transition that nothing completed, with the witness left to mislead a later
reader.

## The repair

`store_finish_import` (`src/app/store_import.c`): when the mode is `STORE_DB`
and the witness names the marker's project and no fork is in progress
(`fork.json` absent; a fork keeps its witness until `fork_snapshot_finish`),
set aside whatever legacy items are still live (no empty archive when they
were all moved) and remove the witness. It runs from `store_import`'s "already
moved" return and from `store_adopt_project` at project open, once, with a
status line when it did something. A death that split the set-aside leaves
two stamped archives, each holding what it moved; nothing is merged or
deleted.

## Measurement, after the repair

| Point | Deaths | Mode at death | After the next process | Archives |
|---|---:|---|---|---:|
| `import.registered` | 1 | files, witness | import resumed, data once, move finished | 1 |
| `import.committed` | 1 | files, witness | same | 1 |
| `import.marked` | 1 | db, witness, nothing aside | finished at the next import | 1 |
| `import.aside_item` | 8 | db, witness, partly aside | finished; two archives when items remained | 2 (hits 1 to 3), 1 (4 to 8: the test project has four of the eight items) |
| `import.set_aside` | 1 | db, witness, all aside | witness removed, no empty archive | 1 |

With the earlier rows: 113 deaths, 1,146 checks, 0 failures, 37 s.

Not driven here: the fork path (`store_fork`), which has its own cleanup and
witness rules and is the matrix's next row; and the app's open path itself,
which calls the same function and is covered by `make uitest-db` on the
acceptance runner.

## Gates

`make test` 95,885 C checks and 161 Python tests, 0 failures; `make dbtest`
18,000, 0; `make analyze` 0 warnings across 88 source files; `make sanitize`
95,885, 0, 0 diagnostics; `make tsan` 48,549, 0, 0 diagnostics; `make crashtest`
1,146 checks, 0 failures. Every exit status 0 (`build/crash/gates4-*.log`,
`build/crash/crashtest.log`). `make uitest` and `uitest-db` could not run on
this Mac (console locked); they run on the acceptance lane.
