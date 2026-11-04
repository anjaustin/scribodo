# Red-team review 008: the project store

**Date:** 2026-11-04
**Scope:** commits `4f82423`, `1a9a705` and `53d14b7`:
- the MariaDB layer and its backends;
- per-module dispatch on `store_mode()`;
- the importer from the file store;
- the `.scribodo/store.json` marker;
- offline pause and reconnection;
- `tools/services.sh`;
- the observer's state in the store;
- the Qdrant index.

**Method:**
- **Independent reviewer:** C probes against throwaway `scribodo_test_*` databases, plus UI scripts against the fake model.
- **Fixes:** each has a regression test where one is possible. Mutation checks were validated by a successful build and a changed binary.

**Result:** 13 findings. Three could mix one project's data into another's, or lose writes without saying so. All are fixed.

## Findings

| # | Sev | Finding | Fix | Verified by |
|---|-----|---------|-----|-------------|
| 1 | H | **A copied project folder merged into the original.** Its import reused the original's record, chat and snapshot ids, so its notes became new versions of the original's. | Ids that belong to another project get their own. These are derived from the project and the original id, so a re-run picks the same one. Snapshot ids include the project. | `dbtest: import_safety` |
| 2 | H | **The marker was trusted blindly.** A copied folder (Finder duplicate, `cp -R`) wrote into the original project. | Marker v2 records the folder's device and inode. A different inode while the original still holds the project is a **copy**: paused, with "Make it its own project" (`store_fork`). A different inode with the original gone is a move across volumes or a restore from backup: the project follows. | `dbtest: import_safety`, `4_copy.scribo` |
| 3 | H | **Writes made during an import were lost:** another window, or the observer, wrote to files that were then set aside. | The import holds an exclusive lock on `.scribodo/import.lock`. While it's held, every other writer sees the project as paused. A fingerprint of the files is compared after each pass, and the import repeats if they changed. The project is adopted before the observer starts. | `dbtest: import_safety` |
| 4 | H | **A lost connection was never reopened.** After a database restart, every write failed until the app was relaunched. | Errors 2002, 2003, 2006, 2013 and 2055 mark the connection broken. `store_db()` reconnects, and goes offline if it can't. | `dbtest: store_ids` (the server kills the connection; the next use reconnects) |
| 5 | M | **History order depended on timestamps.** Saves within one millisecond, or a clock stepped back, reordered it. Importing parsed local-time file names, which is ambiguous across a DST change. | `revisions.seq`, an insertion sequence (migration 003; existing rows are numbered by time). Imports use each snapshot's epoch time plus its milliseconds. | `dbtest: migration`, `backends` |
| 6 | M | **Findings could be lost without a word.** Model text clipped mid-character into the 80-byte subject is invalid UTF-8, which the store refuses, and the result of `records_create_ex` wasn't checked. The automatic chat title had the same cut, so a chat starting with a long non-ASCII line couldn't be saved. | `utf8_clip` everywhere model or author text is shortened. A finding that fails to save leaves its passage unread, so it's read again. | `test_core: utf8_clip` |
| 7 | M | **The root password was on a command line,** and `testdb` interpolated its argument into SQL. | `testdb` names must match `^scribodo_test_[a-z0-9_]+$`. The password is read inside the container from its secret file. The root password was rotated (`services.sh rotate-root`). | code review |
| 8 | M | **The observer rewrote its whole read state after every job.** | Only changes are written, in batches. | `2_resume.scribo` (after a restart nothing is read twice); the batching itself by code review |
| 9 | M | **A marker naming a project the store doesn't have** (store reset, folder from another machine) failed every write. | Detected as **missing**: paused, with "Restore it from its saved files" (`store_restore`, from the newest `legacy-*`). | `dbtest: import_safety` |
| 10 | M | **A store that came up after launch was never used.** | The app probes every 15 s while the store is offline, then connects, adopts the project and refreshes. | code review (no script stops and restarts the database) |
| 11 | M | **Sources weren't scoped to the project.** A finding could link another project's revision, chat or record by id. | A revision must belong to the very document, and chats and related records to the project. Otherwise the source keeps its quote without the link. | `dbtest: backends` |
| 12 | L | **`utf8mb4_bin` pads spaces,** so `Chapter.md` and `Chapter.md ` were one name. | Author text uses `utf8mb4_nopad_bin` (migration 003). Ids keep `utf8mb4_bin`: foreign keys pin them, and they never contain spaces. | `dbtest: migration` |
| 13 | L | **Polish:**<br>• New and Import while paused would have inherited an earlier document's History once the store returned;<br>• Import never set aside an earlier History under a reused name;<br>• store queries ran every 3 s;<br>• "offline" was reported for every pause;<br>• the design doc still described `utf8mb4_bin`;<br>• a search hit without `seq` cast NaN to an integer (UBSan, found while verifying);<br>• chats changed within one second listed in arbitrary order (a flaky `attachments.scribo`, found while verifying). | New and Import wait for the store. Imports set History aside like New. The store is polled every 10 s (this app's own changes refresh at once). The banner and messages say which pause it is. The doc is updated. Payload numbers are range-checked. Chats are ordered by modification time to the nanosecond. | `3_paused.scribo`; `dbtest` under ASan/UBSan; `attachments.scribo` (5 runs) |

## Verification

- **`make dbtest`:** 184 checks (was 114). New: `migration` (003 on a database with data), reconnecting after a dropped connection, History order against tied and reversed timestamps, cross-project sources, and `import_safety`:
  - one importer at a time;
  - a stale path row;
  - folders copied before the import;
  - a copied folder forked;
  - a move across volumes;
  - a missing project restored.

  Clean under ASan/UBSan.
- **`make uitest-db`:** four runs. The new `4_copy.scribo` (10 checks) covers a `cp -R` copy, paused and then made its own project. `3_paused.scribo` now also checks that New waits.
- **`make test`:** 10,017 checks, including `utf8_clip`. `make sanitize` and `make tsan` are clean.
- **`make uitest`** (20 scripts) and **`make cstest`:** pass.
- **Mutation check:** 21 of 21 fixes caught. One ("snapshot ids without the project") was missed at first: forking rewrites snapshots under new names, so the project in the key went unexercised. The test for folders copied before the import catches it.
- **Not covered by a test:**
  - the batching of observer state;
  - the 15 s reconnect probe (no script stops and restarts the database);
  - re-reading a passage whose finding failed to save;
  - ids that stay the same when an import runs again.

  These were checked by code review.

## Remaining risks

- **Forking a copy copies everything the original has at that moment,** including notes made after the folder was copied. The fork can't tell which of the original's records predate the copy.
- **A copy whose original was moved at the same time is taken for the project itself** (the original's path no longer holds it). Both folders then write to one project until the original is opened and found to be the copy.
- **The fingerprint sees sizes and modification times.** A same-size rewrite within the file system's time resolution would go unnoticed. Writers inside the app are locked out; this only concerns outside tools.
- **Migration 003 can't be one transaction** (MariaDB commits DDL as it goes). Each step can run again after a failure.
