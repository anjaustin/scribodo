# GAP-004 — Interruption matrix

**Date:** 2026-11-11. **Status:** in progress; the harness and the migration
rows are done, every other row is open.

The gap ([PRODUCT-GAPS](PRODUCT-GAPS.md), GAP-004): the restart and
fault-injection tests model a crash by editing state (a deleted receipt, a
copied journal, a withdrawn marker) and never kill a process at a transition.
This document is the matrix the gap closes with: every transition that must
survive a crash, the points where the process can die, the invariant the next
process must find, and whether a reproducible process-death test holds it.

## Method

`src/core/crash_point.h` names the moments between a transition's steps:
`crash_point("area.step")`. The product build compiles each call to nothing.
A test binary built with `-DSCRIBODO_CRASH_POINTS` links `tests/crash_point.c`,
where the point selected by `SCRIBODO_CRASH_POINT` (`area.step` for its first
hit, `area.step#N` for its Nth) kills the process with `SIGKILL`: no handler,
no flush, no cleanup. The driver, `tests/test_crash_points.c` (`make
crashtest`), forks and execs itself under each point and hit count, with a
fresh throwaway database per death, until a child finishes without dying,
which is how the driver learns how many points a transition has. It then runs
the recovery the product would run on its next start and checks the row's
invariant. One line per death goes to `build/crash/migration-matrix.tsv`.

## The matrix

| Transition | Points | Invariant on the next start | Status |
|---|---|---|---|
| Schema migration (`db_migrate`): DDL statements, then one receipt per version | `migration.statement` (after each of the 80 statements), `migration.receipt` (after each of the 15 receipts) | `db_migrate` completes to schema 15; `schema_migrations` holds exactly one row per version 1 to 15; every column, index, constraint, check and table (information_schema) is identical to a database migrated once without interruption; a further `db_migrate` changes nothing | **Done 2026-11-11**: 95 deaths, 864 checks, 0 failures, 31 s (`journal/gap004-cycle1_migration.md`) |
| Document save (`doc_io`: temp file, fsync, rename over the target, fsync of the directory) | after the temp is written, after the rename | the target holds the previous or the new content whole, never a mix or a truncation; a temp left behind is removed or ignored on the next save | Open |
| Store writes (chat append, records, revisions, observer state: one transaction each) | before commit, after commit | the row is absent or present once; a reply is never duplicated | Open |
| Legacy import: export, import, marker replacement | before the marker replaces the file-store marker, after | the folder is never unmarked with empty live metadata; the import resumes or the file store stays authoritative | Open; modelled today by file edits (`test_db`, `fork_retry_snapshot`) |
| Fork snapshot archive and cleanup | after the marker commits, before cleanup finishes | the journal is finished on the next open; the sentinel-protected staging is kept | Open; modelled today |
| Disk full and read-only targets | ENOSPC on a small RAM disk; a write-protected target | the save reports and the original is intact | Open |

## Limits

- **Process death, not power loss.** The server keeps running while the client
  dies, so what the server had committed is what a durable server would hold.
  The pinned container (`mariadb:11.4.13`) runs with
  `innodb_flush_log_at_trx_commit=1`, `innodb_doublewrite=ON` and
  `innodb_flush_method=O_DIRECT`, which is the configuration under which a
  committed transaction survives the server's own loss of power; this harness
  does not test that, and the register's residual-risk entry stays.
- **DDL is its own commit.** Every migration statement commits as it runs, so
  the receipt can never be in the same transaction as its DDL; the invariant
  is that every statement is re-runnable, and the matrix holds it for each of
  the 80 statements by dying after it.
- **One process at a time.** The driver never has two processes in the same
  transition; concurrent import ownership is the lock tests' job.
- **The points are in the product code.** They cost nothing in the product
  build (a macro to `(void)0`), and a transition without a point is not in the
  matrix. Adding a row starts by naming its points.
