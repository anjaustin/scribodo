# Saved-revision authority prerequisite

**Status:** implemented prerequisite, not document-memory retrieval. GAP-003
remains P0. [MEMORY-006](MEMORY-006-retrieval-pipeline.md) still retrieves chat
sources only; no document is newly authorized by this change.

## Why a separate receipt is necessary

`documents.current_revision_id` is revision-history state, not savedness. Accepted
AI edits, restores and source snapshots can record unsaved editor text. A save
can also deduplicate against one of those revisions, leaving its original
`origin` unchanged. Neither the current pointer nor filtering revision origins
can establish which immutable revision was last successfully saved.

Migration **010** adds a nullable `documents.saved_revision_id`, with a composite
foreign key that cannot point at another document's revision. It also adds a
persisted SHA-256 `revisions.source_hash` and same-owner unique keys needed for
later typed source units. Hash generation performs a one-time linear backfill;
existing saved pointers stay **NULL**. Historical origin/attachment links do not
retroactively grant saved-document eligibility.

The existing successful-save call site passes `origin="save"` only after
`editor_save()` succeeds. `sdb_revision_record()` now commits that explicit saved
receipt in the same transaction as revision publication, **including deduplicated
saves**. AI/restore/source history does not advance it. Deduplication checks
SHA-256 rather than relying solely on the existing 16-hex history hash. Original
revision content/origin and existing current-revision behavior are preserved.

The composite saved-pointer FK restricts deletion of a referenced revision.
Future revision/document/project deletion code must clear those pointers in the
same explicit deletion transaction; this change does not introduce source GC.
Existing backup/restore compatibility tests pass with the new schema.

## Independent checks, not an authorization shortcut

`sdb_saved_revision_identity()` is a checked SQL-only metadata lookup: database,
project, stable document/revision IDs, SHA-256 and UTF-8 **byte** length. It does
not create documents, load full revision content or grant memory consent.

`memory_saved_file_matches()` supplies the independent filesystem check:

- Worker-only, at most 16 MiB streamed through a fixed 16 KiB buffer.
- Dirty open buffers and remote requests fail before filesystem access.
- Directory descriptor must match the captured project device/inode.
- Basename-only `openat` with no symlink following; missing files, FIFOs and
  directories are rejected without blocking on a FIFO open.
- Exact SQL source byte count/hash, EOF, descriptor/name identity and unchanged
  size/mtime/ctime are checked. A replaced or externally edited file cannot
  inherit an old SQL receipt merely by retaining its name.
- Cancellation is checked between reads. This is **not** an OS storage-I/O
  deadline, nor a promise to lock the filesystem after the check completes.

The future caller must still capture explicit current attachment authorization,
reject remote/edit routes before any local access, and recheck SQL/store/folder
and attachment identities before dispatch. These helpers do not supply that
integration and are not called by the creative-memory planner in this checkpoint.
An unsaved buffer remains explicit foreground input, never a saved source merely
because a revision-history row exists.

## Validation and next work

Real disposable SQL tests cover absent/legacy receipts, first and deduplicated
saves, unsaved revisions, source reuse, ownership/FK checks, byte lengths, hash
collisions in the old history hash, invalid inputs and transactional failure.
Filesystem tests cover exact and oversized caps, Unicode bytes, dirty/remote/
canceled inputs, wrong directory/hash/length, external changes, missing files,
symlinks, FIFOs, directories and empty saved files.
Deterministic read interposition additionally exercises cancellation, same-content
file replacement and a same-length mutation during streaming. Backup rehearsal
asserts a non-NULL saved document/revision/hash receipt survives dump and restore.

Local validation passed `make -j8 all test dbtest`, `make sanitize`, sanitizer
`dbtest`, `make tsan`, ordinary/store UI, real-model checks and backup/restore.
Clang static analysis of the saved-file and revision-store modules reported no
diagnostics. These checks establish neither whole-manuscript retrieval quality
nor bounded server I/O for future revision ingestion.
Hosted CI and trusted database/UI acceptance also passed for implementation
commit `5a89801`.

Still required: frozen attachment revision sets and detach/version fences,
scope-local typed revision units, bounded revision ingestion receipts/outbox,
SQL hydration/acknowledgment for those source types, attachment-filtered recall,
typed source provenance and final admission, then T1/shared generative admission,
optional measured T2, cleanup/deadlines and held-out quality/resource gates.
