# GAP-004, cycle 3: the store-write row

**Date:** 2026-11-11. **Tree:** `98a22a6` plus the changes recorded below.

## The row

Every store write is one transaction that ends in `db_commit` (`src/app/db.c`):
a chat append (conversation upsert, the message, its documents, the
conversation's time, the memory intent), a record version, a revision, a
memory ingest. Two points in `db_commit` name the moments round the COMMIT:
`store.before_commit`, with every statement of the transaction sent and the
COMMIT not, and `store.after_commit`. Because the points are in `db_commit`,
every transaction in the store has them; the driver drives the chat append,
the write an author's reply depends on.

The invariant: the message is absent or present once, with its conversation
row present exactly when a message is; `seq` runs from 0 without a gap or a
duplicate; and the next process's append succeeds and continues the sequence.

## Measurement

`make crashtest` (`build/crash/crashtest.log`, `build/crash/matrix.tsv`): the
child migrates a fresh database, seeds a project and appends two messages
(two commits, so each point is hit twice); the driver checks after each death
and then appends a third message itself.

| Point | Hit | Messages at death | Conversation rows | Next append |
|---|---:|---:|---:|---|
| `store.before_commit` | 1 | 0, seq contiguous | 0 | ok, 1 message |
| `store.before_commit` | 2 | 1 | 1 | ok, 2 |
| `store.after_commit` | 1 | 1 | 1 | ok, 2 |
| `store.after_commit` | 2 | 2 | 1 | ok, 3 |

Each point reports exactly two hits in all, which also records that nothing
else on the append path (the migration, the project insert) runs through
`db_commit`. With the earlier rows: 101 deaths, 926 checks, 0 failures, 38 s.

## Gates

`make test` 95,885 C checks and 161 Python tests, 0 failures; `make dbtest`
18,000, 0; `make analyze` 0 warnings across 88 source files; `make sanitize`
95,885, 0, 0 diagnostics; `make tsan` 48,549, 0, 0 diagnostics; `make crashtest`
926 checks, 0 failures. Every exit status 0 (`build/crash/gates3-*.log`,
`build/crash/crashtest.log`).

## Hosted verdicts

CI passed on `fcf30ba`, `bb0a88e` and `98a22a6`. Trusted acceptance failed
on all three at the scripted UI step, every script within a second, because
the runner's console was locked (TECHNICAL-DEBT, "CI reports after direct
pushes", observed 2026-11-12); the database and crash steps had passed. The
failed jobs are rerun once the console is unlocked, and the verdicts are
recorded with the next cycle.
