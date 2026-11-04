# Store 001: MariaDB as the structured authority

**Status:** design, ahead of implementation (step 4 of the MVP plan).
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

The import is idempotent: rows are keyed by the existing ids, so a crash mid-import resumes.

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

## Open questions

- What the app does when Docker isn't installed or won't start. Proposed: documents and chat still work, and the Explorer shows "Project store unavailable" for notes, canon, findings and history until the services are up. That's the honest degraded mode, since the SOW makes the database the authority.
