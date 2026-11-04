# Store 001: MariaDB as the structured authority

**Status:** implemented, red-teamed (`REDTEAM-008-store.md`). Each module dispatches on `store_mode()`, and the importer runs on first open. Qdrant (step 4b) is implemented too.
**SOW:** §5 (data model), §6 (MariaDB, local-only), §7 (backups, offline), acceptance criteria 1, 6, 8 and 10.

## Why

Today the structured data is in files under `<project>/.scribodo/`: chats, notes, canon, findings, the revision index, observer state and renames. Everything is keyed by document *file name*, so a rename needs a forwarding table (`renames.jsonl`). The SOW calls for MariaDB as the source of truth, with stable IDs, transactions and foreign keys, and for a transactionally consistent export in every backup.

## Services

- **Containers:** MariaDB 11.4 LTS and Qdrant run in Docker (`docker/compose.yml`), pinned by digest and listening on `127.0.0.1` only. Credentials are generated once into `~/Library/Application Support/Scribodo/services/` (folder mode 0700) by `tools/services.sh`. Verified: wrong passwords and missing API keys are refused, and the LAN address can't reach either service.
- **Client:** MariaDB Connector/C 3.4.11 is a pinned submodule, built static against Homebrew's static OpenSSL. It depends only on system libraries.
- **App lifecycle:**
  - At launch the app checks the database. If it's down and Docker is running, it starts the services (`compose up -d`) and shows "Starting local services…".
  - Services keep running when the app quits (`restart: unless-stopped`).
  - Writing documents never waits on the database. Manuscripts are plain files in the project folder.

## Schema (migration 001)

Every table is InnoDB with utf8mb4 (`utf8mb4_uca1400_ai_ci`). IDs are the app's own stable strings (`p-…`, `d-…`, `r-…`, `n-…`, `c-…`, `f-…`, and chat ids), so files, exports and Qdrant payloads all name the same things.

| Table | Holds |
|---|---|
| `schema_migrations(version, applied_at)` | applied migrations |
| `projects(id, name, path, created_at, updated_at)` | a project folder (path unique) |
| `documents(id, project_id, name, current_revision_id, created_at, renamed_at)` | a document. **A rename updates `name`; the id stays.** `(project_id, name)` is unique. |
| `revisions(id, document_id, hash, content LONGTEXT, origin, note, created_at)` | full text per revision (save, ai-edit, before-ai-edit, restore, before-restore, source) |
| `conversations(id, project_id, title, created_at, updated_at)` | chats |
| `messages(id, conversation_id, seq, role, content, error, is_edit, truncated, created_at)` | `(conversation_id, seq)` is unique |
| `message_documents(message_id, document_id)` | documents attached to a message (SOW: "referenced document IDs") |
| `records(id, project_id, kind ENUM('note','canon','finding'), created_at)` | notes, canon entries, findings |
| `record_versions(record_id, ver, status, content, finding_type, subject, relation, object, related_id, related_text, created_at)` | every version (canon history, finding status changes); PK `(record_id, ver)` |
| `source_refs(id, record_id, ver, kind, document_id, revision_id, conversation_id, message_seq, start, len, quote)` | provenance. It points at **ids** (document, revision, conversation), so renames need no forwarding. |
| `entities(id, project_id, name, norm_name, type)` / `links(id, project_id, from_entity, relation, to_entity, finding_id)` | the relationship graph (step 3); each link cites the finding it came from |
| `observer_pieces(document_id, piece_hash)` / `observer_chats(conversation_id, msg, off)` | what the observer has read |
| `jobs(id, project_id, kind, state, attempts, last_error, updated_at)` | background job state |

Every change (a message, a record version, a rename, a revision) is one transaction. Appends become inserts. Version numbers come from `SELECT MAX(ver)+1 … FOR UPDATE` inside the same transaction, which removes the race the JSONL store had to guard with a process-local mutex.

## Migration from the file store

On the first open of a project with the database available:
1. Import `.scribodo/` into MariaDB in one transaction per kind: chats, revisions (text from the snapshot files), notes, canon, findings, observer state.
2. Resolve every document file name, following `renames.jsonl`, to a `documents` row.
3. Move the old files to `.scribodo/legacy-<date>/`, kept read-only for one release so nothing is deleted.

The import is idempotent: rows are keyed by the existing ids, so a crash mid-import resumes. Hardened in red-team 008:
- **One importer at a time.** It holds an exclusive lock on `.scribodo/import.lock`. While the lock is held, every other window and thread sees the project as paused, so nothing is written to files that are about to be set aside.
- **Resumable into the same project.** `.scribodo/import-pending` names the project row being filled. A crashed import continues into that row instead of starting another.
- **Never into someone else's rows.** A project row that already has this path (the folder's marker was lost) is superseded, not reused. Any record or chat id that belongs to another project gets an id of its own, derived from the project and the original id, so a re-run gets the same one.
- **Files that change underneath.** A fingerprint (sizes and times) of the imported files is taken before and after each pass. If anything changed, the import runs again, up to four passes.
- **Times** come from each snapshot's epoch timestamp plus the milliseconds in its file name, not from parsing the local-time name (which is ambiguous across a DST change).

## Copies and missing projects

The marker (`.scribodo/store.json`, version 2) records the project id and the folder's device and inode. On use, it is checked against the store (cached for 30 s per thread):
- **Moved folder** (same inode, new path): the project's path is updated.
- **Copy** (another inode, and the original folder still holds the project): paused, with "Make it its own project". `store_fork` writes the original's data as files into the copy and imports them as a new project with ids of its own. The original is never written to.
- **Missing** (no such project row: the store was reset, or the folder came from another machine): paused, with "Restore it from its saved files". `store_restore` puts back the newest `legacy-*` files and imports them. With none, the project starts again from its documents.

## Backups (SOW §7)

`tools/backup.sh` (and later a menu item) writes:
- a copy of the project folder;
- a `mariadb-dump --single-transaction` of that project's rows.

Qdrant is not backed up. It's rebuilt from MariaDB and the documents (acceptance criterion 10).

## Qdrant (step 4b)

- **Collection:** one per project, `scribodo_<project id>`. Each point is a piece of text (a document paragraph piece, a chat message piece, a note, a canon entry).
- **Vectors:** from Qwen3-Embedding-0.6B (F16 GGUF, Apache-2.0, 1,024 dimensions), served by a second llama-server in embedding mode on its own Unix socket.
- **Payload:**
  - source ids and range;
  - `authority`: `canon`, `note`, `draft` (documents) or `chat`;
  - the canon status.

  Retrieval filters on `authority`, so canon is never confused with draft text (acceptance criterion 10).
- **Rebuild:** derived data only. A "Rebuild index" action drops the collection and re-embeds from MariaDB.

## When the database is down

There is one authority per project, and nothing to merge:
- **A project not yet moved to the store** (no `.scribodo/store.json`) keeps working on its local files, exactly as before. It's imported the first time it's opened with the database up.
- **A project in the store, with the database down:** documents stay fully editable and saveable, since they're plain files. Notes, canon, chats, History and the observer pause behind a banner: "Project store offline — your documents are unaffected". They resume when the database is back.
- **Self-repair:** at launch, if the database is unreachable and Docker is running, the app runs `tools/services.sh up` itself and retries.

Merging offline file writes into the database later was rejected. Record version numbers and message sequences would restart in the new files and collide with the database's, needing renumbering rules that are easy to get wrong.

**Per-thread connections:** the UI and the observer worker each get their own connection (thread-local, opened on first use), because a MariaDB connection must not be shared between threads.

**Byte-exact names:** names and other author text use `utf8mb4_nopad_bin` (migration 003). The server's default collation ignores case and accents, which would make `a.md`/`A.md` and `é.md`/`e.md` collide on case-sensitive volumes. Plain `utf8mb4_bin` still ignores trailing spaces, so `Chapter.md` and `Chapter.md ` would be one name. Generated ids keep `utf8mb4_bin`: foreign keys pin their collation, and they never contain spaces.

**History order** is the insertion sequence `revisions.seq` (migration 003), not a timestamp. Two saves in one millisecond, or a clock stepped back, can't reorder History.
