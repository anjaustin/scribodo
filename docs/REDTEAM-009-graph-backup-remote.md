# Red-team review 009: the entity graph, backup and recovery, the remote model

**Date:** 2026-11-04
**Scope:** commits `68b9641` (entities), `f9833d9` (backup and recovery) and `377154e` (the remote model). Also the follow-ups from the acceptance gaps: in-app backup and restore, remote messages marked, the offline sandbox, and tighter real-model checks.

**Method:**
- **Independent reviewer:** C probes against throwaway `scribodo_test_*` databases, crafted backups, UI scripts with the fake model, and a stand-in provider.
- **Fixes:** each has a regression test where one is possible. Mutation checks were validated by a successful build and a changed binary (or, for scripts, by the changed file).

**Result:** 8 confirmed findings, one confirmed by reading the code, and 5 suspicions. The worst: a crafted backup could run a root shell and arbitrary SQL in the database container, and write files wherever its manifest said. All confirmed findings are fixed; the suspicions are fixed or documented.

## Findings

| # | Sev | Finding | Fix | Verified by |
|---|-----|---------|-----|-------------|
| 1 | H | **A crafted backup took over the database and the file system.** `restore.sh` piped the dump into `mariadb -uroot`. The client's `\!` ran as root in the container, the SQL could reach any database, and the folder went to whatever path the manifest named. Its checksums were recomputed by the attacker. | The dump must be plain `mariadb-dump` output (header and completion line). It's loaded in the client's sandbox mode as a one-time user granted only on the target database, with the password made and used inside the container. The destination is never read from the backup: `--to` is required. Databases are limited to `scribodo` and `scribodo_test_*`. The docs say plainly that checksums detect damage, not tampering. | `backuptest`: no `--to`, a shell escape (checked inside the container), another database, a non-dump file and a `mysql` target are all refused |
| 2 | M | **A first name and a full name in one relationship failed every import.** Upserting the object folded the subject's entity away, and the link then failed its foreign key. The graph backfill ran inside the import transaction, so the whole import rolled back, every time. | The subject is looked up again after the object, folding is one transaction, and a graph failure never fails an import (the graph is derived data). | `dbtest: entities_redteam` (live and imported) |
| 3 | M | **Duplicate links.** "One link per finding" was check-then-insert, and the UI's backfill and the observer each have a connection, so two connections inserted 571 links for 300 findings. | Migration 006 removes duplicates and adds a unique key, then `INSERT IGNORE`. | `dbtest`: two threads over the same 60 findings make exactly 60 links |
| 4 | M | **After a restore, the semantic index still held content newer than the backup.** Canon that no longer existed was retrieved for conflict checks. | `restore.sh` drops the restored database's collections, and the app rebuilds them from the store. | `backuptest`: `--replace` drops the stale collection |
| 5 | M | **An interrupted rebuild never finished.** An embedding failure after the drop left an empty index whose marks said "done", so documents and chats never came back. Per-piece failures were skipped silently. | `projects.index_rebuild_pending` is set before the drop and cleared only when every piece succeeded. A later sync resumes it. Pieces are cut on character boundaries. | `dbtest: semantic`: the embedder is down mid-rebuild, then recovers, and documents are found again; afterwards the flag is clear |
| 6 | M | **Switching to the remote model sent the whole local conversation.** Earlier local turns, and edit turns with rewritten passages, went along, while the notice suggested "from now on". | Remote requests carry only messages already exchanged with the remote, plus the new one. The notice says "new chat messages … Earlier messages and the observer stay on this Mac." | `test_llm: remote` (a private local turn never sent; the remote exchange is the context); `remotetest` (the provider never saw it) |
| 7 | M | **The graph backfill froze the UI for seconds,** about 6 s at 4,000 findings, every session and on every project switch. The entity list cost 165 ms on each findings refresh. | The backfill runs on a background thread, once per project (`projects.graph_built`). Each record now stores its current status (indexed), so the list takes 10 ms and the detail view 2 ms at 4,000 findings. | probe timings; `dbtest` |
| 8 | L–M | **The checksums didn't cover added files or links.** | `verify` rejects links and other non-regular entries, and any file not in the list. Names with newlines or backslashes are still handled. | `backuptest` (a planted file, a link); a manual check with odd names |
| 9 | L–M | **A click could act on a different entity than the one shown** when findings arrived in the same frame. *(Found by reading the code.)* | Clicks use the ids captured when the rows were laid out, never indices into refreshed lists. | code review; `5_entities.scribo` |

## Suspicions

| Suspicion | Outcome |
|-----------|---------|
| Restoring while the app runs | `restore.sh` refuses while Scribodo is running. |
| `--replace` rolls back every project | Documented in the script and the design doc. In-app restore is per-project and never touches the database. |
| An endless reply line | Lines are capped at 4 MB; the reply then stops. |
| `SSLKEYLOGFILE` | Unset at startup, so TLS secrets are never written out. |
| `remote.json` naming any 0600 file as the key | `remote.json` must be yours and not writable by others. The key file must be a regular file of yours, and never one of Scribodo's own service secrets. |

## Known limits (by design, documented)

- **Folding a first name into a full name depends on arrival order.** If "Anna" folds into "Anna Berg" and "Anna Lund" appears later, the earlier "Anna" mentions stay with Berg.
- **Only cased scripts are recognised.** Names in Latin, Greek and Cyrillic are; scripts without capitals (CJK and others) aren't.

## Verification

- **`make dbtest`:** 324 checks (was 238). New: `entities_redteam` (a subject folded by its own object, live and imported; two connections linking the same findings; Greek and Cyrillic names), the resumed rebuild, and export dates and snapshots.
- **`make backuptest`:** the disaster rehearsal, plus every crafted backup:
  - no `--to`;
  - a planted file;
  - a link;
  - another database;
  - a shell escape, checked inside the container;
  - a file that isn't a dump;
  - a `mysql` target;
  - a stale index after `--replace`.
- **`make remotetest`:** the provider never sees the local exchange made before the switch.
- **`make test`:** 10,062 checks. `uitest`, `uitest-db` (6 runs, including in-app backup and restore), `cstest`, `sanitize` and `tsan` all pass.
- **`make offlinetest`** (real models, no network): passes.
- **Mutation check:** 21 of 21 caught.
  - "Subject not re-resolved" was missed at first. `INSERT IGNORE` on links hid the foreign-key error as well as the duplicate, so the bug went silent. Links now tolerate only the duplicate, and the test calls the graph update directly.
