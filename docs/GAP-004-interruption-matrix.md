# GAP-004 — Interruption matrix

**Date:** 2026-11-11. **Status:** in progress; the harness, the migration rows, the save row and
the store-write row are done; the marker, cleanup and disk rows are open.

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
invariant. One line per death goes to `build/crash/matrix.tsv`.

## The matrix

| Transition | Points | Invariant on the next start | Status |
|---|---|---|---|
| Schema migration (`db_migrate`): DDL statements, then one receipt per version | `migration.statement` (after each of the 80 statements), `migration.receipt` (after each of the 15 receipts) | `db_migrate` completes to schema 15; `schema_migrations` holds exactly one row per version 1 to 15; every column, index, constraint, check and table (information_schema) is identical to a database migrated once without interruption; a further `db_migrate` changes nothing | **Done 2026-11-11**: 95 deaths, 864 checks, 0 failures, 31 s (`journal/gap004-cycle1_migration.md`) |
| Document save (`doc_io`: temp file, fsync, rename over the target, fsync of the directory) | `save.temp_written` (the new content complete and fsynced in the temp, target untouched), `save.renamed` (target replaced, directory not yet fsynced) | the target holds the previous or the new content whole, never a mix, a truncation or nothing; the next save succeeds and the target holds it whole | **Done 2026-11-11**: 2 deaths, both whole, next save fine. **Finding:** a death after `save.temp_written` leaves `doc.md.scribodo-XXXXXX` (mode 0600, the complete new text) beside the document, and nothing removes or reads it afterwards; see below (`journal/gap004-cycle2_save.md`) |
| Store writes (chat append, records, revisions, memory ingest: one transaction each, through `db_commit`) | `store.before_commit` (the transaction's statements sent, COMMIT not), `store.after_commit` | the message is absent or present once with its conversation; `seq` runs 0..n-1 with no gap or duplicate; the next process's append continues the sequence | **Done 2026-11-11** on the chat append: 4 deaths (two appends, each point), 0 or 1 or 2 messages exactly as the commit count says, next append continues (`journal/gap004-cycle3_store.md`). The points sit in `db_commit`, so every store transaction has them; the driver drives the chat append |
| Legacy import: export, import, marker replacement | before the marker replaces the file-store marker, after | the folder is never unmarked with empty live metadata; the import resumes or the file store stays authoritative | Open; modelled today by file edits (`test_db`, `fork_retry_snapshot`) |
| Fork snapshot archive and cleanup | after the marker commits, before cleanup finishes | the journal is finished on the next open; the sentinel-protected staging is kept | Open; modelled today |
| Disk full and read-only targets | ENOSPC on a small RAM disk; a write-protected target | the save reports and the original is intact | Open |

## Findings awaiting a decision

**Stray save temps.** A process that dies after the temp file is complete and
before the rename leaves `<document>.scribodo-XXXXXX` beside the document. It
holds the author's complete text as of that save, readable only by the owner,
and the product neither removes it nor offers it: the next save creates a
fresh temp and the stray stays for good, one per such crash. The author sees
an unfamiliar file in their folder. The matrix holds the invariant (the
document itself is whole), so this is not data loss; it is unmanaged
author data. Options, none taken here: remove stale temps of a target on the
next successful save of that target (loses a copy of text the author may not
have elsewhere); keep them but name them so the author can read them; or
read them on load and offer the newer text. The author's rule (delete
nothing, only archive) argues against the first.

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
