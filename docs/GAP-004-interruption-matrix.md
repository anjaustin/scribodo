# GAP-004 — Interruption matrix

**Date:** 2026-11-11. **Status:** complete for the six transitions the gap named and the app's
small-file writer (2026-11-12):
129 matrix rows, 125 of them process deaths, 0 failures (2026-11-12, after migration 016; 131 rows, 127 of them process deaths, and 0 failures
on 2026-11-14 after migration 017 added the messages table's `is_note` column, `build/observer/crashtest-017.log`); two unfinished-move
defects and one misleading refusal found and repaired; the one finding that
awaited a decision (stray save temps) was decided and built on 2026-11-12. The
limits below stay, and the register's residual-risk entry with them.

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
| Schema migration (`db_migrate`): DDL statements, then one receipt per version | `migration.statement` (after each of the 83 statements), `migration.receipt` (after each of the 17 receipts) | `db_migrate` completes to schema 17; `schema_migrations` holds exactly one row per version 1 to 17; every column, index, constraint, check and table (information_schema) is identical to a database migrated once without interruption; a further `db_migrate` changes nothing | **Done 2026-11-11**: 95 deaths, 864 checks, 0 failures, 31 s (`journal/gap004-cycle1_migration.md`) |
| Document save (`doc_io`: temp file, fsync, rename over the target, fsync of the directory) | `save.temp_written` (the new content complete and fsynced in the temp, target untouched), `save.renamed` (target replaced, directory not yet fsynced) | the target holds the previous or the new content whole, never a mix, a truncation or nothing; the next save succeeds and the target holds it whole | **Done 2026-11-11**: 2 deaths, both whole, next save fine. **Finding, decided and built 2026-11-12:** a death after `save.temp_written` leaves `doc.md.scribodo-XXXXXX` (mode copied from the document, the complete new text) beside the document; the next open now keeps it as `doc (Scribodo recovered <stamp>).md` and says so, and the driver checks that the recovered file holds the new text whole (`journal/gap004-cycle2_save.md`, `journal/gap004-cycle8_recover.md`) |
| Store writes (chat append, records, revisions, memory ingest: one transaction each, through `db_commit`) | `store.before_commit` (the transaction's statements sent, COMMIT not), `store.after_commit` | the message is absent or present once with its conversation; `seq` runs 0..n-1 with no gap or duplicate; the next process's append continues the sequence | **Done 2026-11-11** on the chat append: 4 deaths (two appends, each point), 0 or 1 or 2 messages exactly as the commit count says, next append continues (`journal/gap004-cycle3_store.md`). The points sit in `db_commit`, so every store transaction has them; the driver drives the chat append |
| Legacy import (`store_import`): register the project and write the witness, import in one transaction, write the marker, set the legacy files aside, remove the witness | `import.registered`, `import.committed`, `import.marked`, `import.aside_item` (after each of the 8 items), `import.set_aside` | the folder is never unmarked with empty live metadata: without a marker the files stay authoritative and the next import resumes under the witness's project; with a marker the store is authoritative; the data is read once (2 messages, 1 note with 2 versions, 1 canon), every legacy item ends up in a stamped archive and the witness is gone | **Done 2026-11-12**: 12 deaths, 0 failures after a repair. **Finding, repaired:** a death after the marker left the legacy files live and the witness in place, and the next open never finished the move (the store was authoritative, so nothing asked); `store_finish_import` now finishes it at open and on a repeated import, only when the witness names the marker's project and no fork is in progress (`journal/gap004-cycle4_import.md`) |
| Fork of a copied folder (`store_fork`): open the journal, export the store's data to a staging folder, import it as a new project (the import row's points), close the journal (archive the staging, remove witness and journal) | `fork.journal_opened`, `fork.exported`, the import points, `fork.imported` (marker written, journal still open) | a copy stays a paused copy until its marker is replaced, then it is a project in the store with its own id, never a plain file store; the copy's data is read once and the original is untouched; the journal is closed (no staging, witness or `fork.json`) after the next process acts as the app would (fork a copy again; open a project) | **Done 2026-11-12**: 7 deaths, 0 failures after a repair. **Finding, repaired:** a death after the marker left the staging, witness and journal in place, and a plain open never closed them (only a further fork of a copy did); `store_finish_import` now closes a journal whose target is the marker's project (`journal/gap004-cycle5_fork.md`) |
| The app's small files (`file_text_write_atomic`, `core/file_text.c`: the one writer behind the profile, the recent-projects list, the model settings, saved revisions, the store marker, the import witness and the observer state) | `atomic.temp_written`, `atomic.renamed` | as the save row: the target holds the previous or the new content whole; the next write succeeds | **Done 2026-11-12**: 2 deaths, both whole; a temp is left after the first, as for the save; it is kept and not offered, see below (`journal/gap004-cycle7_atomic.md`) |
| Disk full and read-only targets (not a death: a 2 MB RAM disk filled to the last byte, then a write-protected target) | `disk.full` save and import, `disk.read_only` save | the save is refused with the cause and the document is intact with no temp left; the import is refused with its cause and the files stay authoritative with their data intact; once space is back the same import succeeds with the data once; a write-protected target is refused as such | **Done 2026-11-12**: 4 rows, all held. **Finding, repaired:** an import on a full disk was refused as "being moved into the store by another window" because its lock file could not be created; the refusal now names the cause (`journal/gap004-cycle6_disk.md`) |

## Findings decided

**Stray save temps (decided 2026-11-12: keep them and offer them on load).** A
process that dies after the temp file is complete and before the rename leaves
`<document>.scribodo-XXXXXX` beside the document, holding the author's complete
text as of that save. Before the decision the product neither removed nor
offered it: the next save created a fresh temp and the stray stayed for good,
an unfamiliar file in the author's folder. The matrix held the invariant (the
document itself is whole), so this was not data loss; it was unmanaged author
data. The three options were to remove stale temps on the next successful save
(loses a copy of text the author may not have elsewhere; the delete-nothing
rule argues against it), to keep them under a readable name, or to read them on
load and offer the newer text.

The author chose to keep them and offer them on load. `doc_recover_strays`
(`core/doc_io.c`) runs on every document load: each regular file named exactly
`<basename>.scribodo-` plus six mkstemp characters beside the real document is
moved, never over an existing file, to `<name> (Scribodo recovered
<stamp>).<ext>` (the stamp is the temp's modification time, a counter follows
when the name is taken), which is the same shape as the conflict copy the
product already makes when a file changed on disk; it lists in the Explorer
and opens like any document. The status line names it and says whether its
text differs from the document's. Nothing is deleted: the move is a rename in
the same folder, and the strays' bytes are what they were. The crash driver
now runs that recovery after the death at `save.temp_written` and checks that
the recovered file holds the new text whole and that no temp remains; the
unit test `test_doc_io: recover_strays` covers look-alike names, a taken name,
a stray with the document's own bytes, a document without an extension and
one opened through a symlink; `tests/ui/files.scribo` opens a document with a
stray beside it in the real app and checks the status and the folder.

The app's small files (`<file>.tmp-XXXXXX` beside the profile, the settings or
a marker) hold no author prose and are not a document anyone would open, so
the same decision applies as "keep" only: nothing removes them, nothing offers
them. A later start reads the target, which is whole.

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
  the 82 statements by dying after it.
- **One process at a time.** The driver never has two processes in the same
  transition; concurrent import ownership is the lock tests' job.
- **The points are in the product code.** They cost nothing in the product
  build (a macro to `(void)0`), and a transition without a point is not in the
  matrix. Adding a row starts by naming its points.
