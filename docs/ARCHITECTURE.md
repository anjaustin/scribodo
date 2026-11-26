# Scribodo end to end

**Source-traced:** 2026-11-25, at `69b98abf1ecf86c560b435a209327175c182eab1`.
This is a map of the implementation and its product boundaries, not a fresh real-model or UI acceptance run.
Known defects from the accompanying review remain open in [AUDIT-005](AUDIT-005-repository-audit.md).

## 1. What the product is for

Scribodo is an author's local workbench for developing a book: thinking in conversation, writing in documents,
reviewing possible changes, and deciding which assertions belong to the book's canon.
It does not require an outline or pre-established writing mode, and it is not an autonomous book-writing agent.

The important boundary is **author authority**:

- Conversation can contain possibilities, abandoned ideas and model mistakes.
- A manuscript is authored text, not automatically a list of established facts.
- Notes are editable, non-authoritative records.
- Canon is an assertion the author explicitly committed; it can later be revised or retracted.
- Observer findings and generated memory aids remain suggestions, never automatic canon.

The memory work serves that product, not a claim of unlimited model intelligence. Its three independent
problems are **retention**, **selection**, and **faithful use**. Keeping an original does not guarantee
retrieving it, and retrieving it does not guarantee the model understands it.
See [MEMORY-015](MEMORY-015-effective-memory-contract.md) for the contract and
[PRODUCT-GAPS](PRODUCT-GAPS.md) for the remaining product work.

## 2. The system at a glance

```text
Author
  |
  v
Native workbench: raylib + Clay + custom widgets/editor
  |                         |                         |
  |                         |                         +-- Recall panel
  |                         |                             exact stored text,
  |                         |                             lexical search, no model
  |                         |
  |                         +-- Chat worker
  |                             foreground context + optional local memory
  |                             plain composer OR verified-answer pipeline
  |                             local generation OR explicitly selected remote
  |
  +-- Document buffer --> explicit save --> manuscript .md/.txt files
  |         |
  |         +-- undo log; reviewed edits; recovery drafts
  |
  +-- ProjectStore routing
            |
            +-- files: .scribodo/ logs and revision snapshots
            +-- database: local MariaDB, structured authority after import
            +-- paused: refuse structured writes; do not fork a second history

Saved documents/messages --> observer worker --> sourced findings --> author action
                                                   |
                                                   +-- derived entities/links

Consented conversation + current saved attachments
  --> memory worker --> SQL units/receipts/outbox --> embeddings --> Qdrant
                                                           derived locators only

Local models: LFM3 generation | Qwen3 embeddings | Qwen3 reranker
```

There are two durable authorities to distinguish: manuscript files for the actual saved documents, and the
project's selected structured backend for chats, History, records and job state. Qdrant is not an authority.
An editor buffer can be newer than both the manuscript file and its saved-revision identity.

## 3. Launch, services and shutdown

`src/main.c:main()` composes the app in explicit phases from `src/app/app_lifecycle.c`:

1. Prepare environment/test isolation before starting threads (`startup_env_prepare`).
2. Create the store manager, controllers and non-window jobs.
3. Open the native window, fonts, widgets and Clay layout arena.
4. Load model settings, estimate available memory and start the local model supervisors.
5. Create the chat session, editor and views; load any remote configuration without selecting it.
6. Load recent projects, then open the requested path or most recent project/document.
7. Tick model supervisors, poll worker results, handle input, update layout and draw each frame.

MariaDB and Qdrant are managed by `tools/services.sh`, launched/probed through
`src/app/service_supervisor.c`. Native services are recommended on constrained Macs; Docker remains an
alternative. Native mode selection preserves an existing Docker store rather than silently starting an
empty native replacement. Defaults come from `tools/services.defaults` and the generated/shared app defaults.

The model supervisors in `src/llm/server.c` validate pinned models, use private Unix sockets and fresh API
keys, wait for health, and restart crashed servers with backoff. The app reaps identifiable orphan servers
before sizing a replacement after a crash. Context is fitted to available memory rather than assuming the
model's advertised maximum fits.

Shutdown closes generation admission, cancels/joins borrowers and workers, closes their thread-owned DB
connections, then destroys models, views and the window. Model-server lifetime belongs to the app.
The service supervisor's destruction is not a `services.sh down` of the persistent database services.

**Sources:** `src/main.c`, `src/app/app_lifecycle.c`, `src/app/startup_env.c`,
`src/app/service_supervisor.c`, `src/app/local_models.c`, `src/llm/server.c`, `src/llm/memfit.c`.

## 4. Opening a project and selecting its authority

A project is a local directory. The document list is derived from supported files in that directory;
the database does not replace the manuscript files.

`store_mode()` and `ProjectStore` select among:

| State | Behavior |
|---|---|
| No store marker and no import in progress | File-backed structured data under `.scribodo/` |
| Valid marker, verified project identity and reachable database | MariaDB-backed structured data |
| Marked project with offline database, malformed marker, conflicting copy or missing registration | Structured operations pause |

A marked project does **not** switch back to file history on database failure. That is deliberate protection
against two competing authorities. Document editing/saving remains a filesystem operation.

When the store is available, a legacy project's first adoption runs on the import worker. File writers
coordinate through the import lock; the importer reads histories, chats, notes, canon, findings and observer
state, preserves/rebases identities where necessary, and commits before publishing the store marker.
Old structured files are retained under `.scribodo/legacy-*`. Interrupted transitions have explicit
completion/retry handling.

The marker carries a project ID and directory identity. Moving the original folder and opening a copy while
the original still exists are different operations. A copy is not permission to write into the original
project's database identity; explicit fork/recovery paths exist.

**Sources:** `src/app/store.c:store_mode`, `src/app/project_store.c:resolve`,
`src/app/store_import.c`, `src/app/import_job.c`, `src/main.c:store_adopt_project`,
`src/main.c:import_poll`. Representative contracts: `tests/ui/store/1_move_and_use.scribo`,
`2_resume.scribo`, `3_paused.scribo`, `4_copy.scribo`.

## 5. Writing, saving and recovering a document

The editor stores Unicode codepoints in a gap buffer. stb_textedit supplies navigation/selection logic;
Scribodo owns grouped undo/redo, wrapping, rendering, file state and tracked proposal anchors.
Files are decoded/encoded as UTF-8 at the boundary. Codepoint positions in UI/source references must not be
confused with the byte offsets used by memory ingestion.

`save()` first calls `editor_save()`:

1. Check read-only state and the expected on-disk identity.
2. Encode the buffer with its BOM/newline policy.
3. Write and sync an exclusive temporary sibling.
4. Recheck the destination, then atomically rename into place.
5. Update the editor's saved state.

If another program changed the file, the editor tries to retain the author's text as a conflict copy and
requires a separate, short-lived overwrite confirmation. Invalid UTF-8 is opened read-only rather than
silently round-tripped through replacement characters.

After a successful file save, the app separately records a `save` revision and pokes the observer.
These are not one atomic transaction across the filesystem and MariaDB. In particular, a file can have
saved successfully while recording its History fails; the app reports the latter.

File-backed History uses the complete manuscript basename, with exceptional old-format names under
`.scribodo/revisions/.exact-v1/`. A shared resolver rejects ambiguous legacy lossy buckets; a path-format
tag never proves old ownership. Rename/archive publication is serialized by the revisions-root lock:
a durable `.pending-move.json` receipt retains full names, manuscript/history inode+device identities,
an operation ID and event time. Replay checks identities, moves without replacing an unrelated object,
syncs parent directories, publishes the dated rename idempotently, then removes/syncs the receipt.
Project/document opens, source-name resolution and History reads check recovery first. Conflicting
receipts remain intact and make the operation unavailable; manuscript hardlinks are refused.
See [COMPOSITION-001 revision identity](COMPOSITION-001-revision-identity.md) for compatibility and
verification limits. This is not a power-loss or hostile concurrent ancestor-replacement guarantee.

Dirty buffers also get periodic sibling recovery drafts through `DraftController`, normally on its worker.
On a later load, stray save/draft temps are retained under recovered-document names. This is not the same
as saving the manuscript, and periodic drafts cannot promise to retain every keystroke before a crash.

**Sources:** `src/ui/editor.c`, `src/core/text_buffer.c`, `src/core/undo.c`, `src/core/wrap.c`,
`src/core/doc_io.c`, `src/app/draft_controller.c`, `src/main.c:save`.
Regressions: `tests/test_doc_io.c`, `tests/test_editor.c`, `tests/test_editor_faults.c`,
`tests/test_draft_controller.c`.

## 6. A normal chat turn

`send_chat()` in `src/app/chat_workflow.c` checks the active project, pending history/memory controls and
model state. It creates/binds a conversation if necessary, and snapshots the documents explicitly attached
to that turn. The open document comes from its current buffer; other attachments come from disk.
Attachment names are persisted with the user message, not inferred later from whatever file happens to be open.

`chat_send()`/`send_common()` then:

1. Acquire foreground generation intent when using the shared local model.
2. Copy request inputs and append the user message plus a streaming assistant placeholder.
3. Start a worker to prepare bounded history/context and execute inference.
4. Publish streaming text under the session lock.
5. Finalize error/stopped/truncated state and any answer checks.
6. Let `chat_poll()` invoke persistence callbacks on the UI thread.

The user's turn is eligible for its persistence callback before the assistant finishes.
`on_message_done()` stores messages with the conversation's bound project, records failure state on a
refused append, and pokes the observer after stored assistant turns.

There are three separate bounds: the durable transcript, the display window, and the request's model
history. DB display windows are paged by stable sequence, not in-memory array position; request history
copies at most 64 messages/64 KiB before token budgeting. Display excerpts are not complete model-history turns.
Older originals remain in the store even when no longer resident or admitted to a request.

The request builder reserves reply room, treats attached text as reference material, neutralizes model
control-token strings and removes earlier reasoning. These mechanisms do not establish immunity to semantic
prompt injection or factual mistakes.

**Sources:** `src/app/chat_workflow.c:send_chat`, `src/llm/chat.c:send_common`,
`build_begin`, `build_base`, `build_memory`, `build_finish`, `server_worker`, `chat_poll`;
`src/llm/chat_history.c`, `src/app/store_chat_history.c`, `src/app/chat_history_job.c`.
Tests include `tests/test_llm.c`, `tests/test_chat_history.c`, `tests/test_chat_history_job.c`,
`tests/ui/store/7_chat_windows.scribo`, `10_async_history.scribo`.

## 7. Four different things often called “memory”

### A. Durable originals and History

Messages, immutable revisions and record versions preserve what was actually written. A `source` snapshot,
an accepted-edit revision and a `save` revision are not interchangeable. In the DB backend,
`documents.saved_revision_id` explicitly identifies the saved revision; the newest History entry may still
represent unsaved editor text.

### B. Deterministic Recall

The Recall panel searches current committed canon project-wide, plus eligible stored excerpts under enabled
conversation memory scopes. It performs lexical/full-text/short-term matching and exact source verification,
not vector search or generation. It can display both author and assistant chat excerpts, labeled as such.
Its candidates, conversations and rows are bounded, and it reports coverage/limits rather than claiming a
negative result proves absence from the book. It can show indexed historical document revisions labeled
with whether they are still the current saved revision.

**Sources:** `src/app/recall.c`, `src/app/recall_job.c`, `src/ui/recall_view.c`,
`tests/ui/store/15_recall.scribo`.

### C. Local conversation retrieval

Memory availability at startup does not grant consent. The author enables a scope per stored conversation.
`MemoryJob` serializes consent controls separately from navigation, ingests original sources in bounded
steps, counts tokens, writes SQL units/receipts and delivers a fenced outbox to the vector index.

Saved-document retrieval additionally requires a current explicit attachment, its saved-revision identity,
and a verification that the local file matches that source. Dirty, detached, missing or changed document
identities do not authorize old saved content for the next request. Verification progresses on a worker;
foreground preparation uses bounded cached proofs and rechecks, not an unbounded manuscript hash.

The planner combines lexical and semantic candidate ranks, hydrates exact SQL source text, enforces scope,
generation, tokenizer, source/hash and recent-tail exclusions, and fits an optional reference packet.
Normal planner caps include 16 KiB/4,096 tokens. A 500 ms preparation budget is separate from the much longer
verified-pipeline budget. Before recall-packet dispatch, identities and sources are checked again.

Qdrant hits on this path are **untrusted locators**, not trusted payload text. Missing collections require
SQL generation/reseed work before rebuilding; stale vector responses cannot authorize a source.
Failure can produce a disclosed recent-context-only request, or reject admission rather than send stale
material. It never automatically turns into remote retrieval.

**Sources:** `src/app/memory_job.c`, `src/app/memory_store.c`, `src/app/memory_document_store.c`,
`src/app/memory_documents.c`, `src/app/memory_saved_file.c`, `src/llm/context_planner.c`,
`src/llm/memory_vectors.h` (implemented in `src/llm/semantic.c`), `src/llm/memory_budget.c`.
Contracts: `tests/ui/store/12_memory_recall.scribo`, `13_memory_documents.scribo`.

### D. Verified-answer pipeline

With consent, available saved attachments and the configured reranker, the default local path can replace
the ordinary recalled packet/document previews with a narrower evidence flow:

1. Gather lexical and vector candidates (up to 32 from each channel).
2. Hydrate from SQL and keep manuscript units, not chat messages citing themselves.
3. Score them with Qwen3-Reranker; by default rank the whole request, not split question parts.
4. Select up to two units and ask the local generator to extract stating spans.
5. Verify those spans and return the manuscript's own bytes, not the model's rewritten quotation.
6. Compose from the verified block and a short conversation window.
7. For qualifying question turns, extract a claim and check anchors, quotations and supported relation shapes.
   A failed supported check can replace the final reply with a withholding notice plus source spans.
8. Store the as-of source/check receipt separately from model history.

This is not a proof that the final prose is true. The claim extractor is a model call; the software checks
cover bounded classes; an unavailable claim verdict is not automatically withholding. The post-generation
checks run after streaming, not as a guarantee that unsupported provisional text can never be displayed.
Note-marked turns do not arm the question gate. The question heuristic and the quality limits are explicit.

A memory-on chat without attached documents can use chat recall instead; missing/unready reranking has
explicit fallback/notices. Neither “pipeline enabled” nor “memory on” means every turn successfully ran
every verification stage.

**Sources:** `src/llm/answer_pipeline.c:answer_pipeline_run`,
`src/llm/chat.c:memory_pipeline`, `gate_arm`, `gate_apply`,
`src/llm/rerank.c`, `src/llm/answer_receipt.h`, `tests/test_answer_pipeline.c`.

## 8. Reviewed edits are a different path

Selecting a passage and requesting an edit captures its original text and a tracked editor anchor.
The edit request uses an editing instruction/passage, not the ordinary conversation-memory route.
A completed reply becomes a reviewable diff; a stopped partial reply does not become a replacement proposal.

Accept checks the document identity, editability, live anchor and original text. It records a before-edit
revision **before** replacing the range, applies one undoable edit, then records the after-edit revision.
A failed before revision prevents application; a failed after revision is reported while retaining the
already-applied editor change. Reject changes no manuscript text.

**Accepting is not saving the file.** Restoring a History revision likewise edits the buffer undoably and
protects the previous text first; it does not silently overwrite the manuscript on disk.

**Sources:** `src/app/reviewed_edit_controller.c`, `src/main.c:accept_proposal`, `restore_revision`,
`src/app/chat_workflow.c:on_message_done`, `src/ui/review_view.c`, `src/core/diff.c`,
`tests/ui/edits.scribo`.

## 9. The observer and author decisions

The observer reads stored documents/messages independently of the currently attached chat documents.
Its read state is paragraph/piece hashes for documents and progress within each conversation.
It discovers documents first, then eligible non-archived chats. Observer-ignore and memory consent are
different settings: enabling/disabling retrieval does not itself define what the observer reads.

A job extracts bounded candidate findings, locates their evidence in the source, filters conversation
meta-content/names, deduplicates, finds relevant committed canon/notes, and performs conflict checks.
Document evidence gets an immutable source snapshot. Before publication the job rechecks generation/fences
and existing findings; progress is not advanced past a failed finding write.

The observer may use the older project-wide semantic index for canon/note matching and index its read text.
That index is distinct from consent-scoped memory units/outboxes; sharing Qdrant and an embedder does not
make their authorization protocols identical.

The same local LFM3 serves both chat and observation. Foreground generation has priority through the shared
gate and chat-active cancellation. Repeated interruption increases the observer's required idle delay.
This reduces wasted calls but does not promise backlog drainage during continuous chat.

The author then chooses:

- **Discuss:** append the finding to the visible composer; only sending makes it conversation input.
- **Note:** create a noncanonical versioned record with sources.
- **Canon:** open a draft; only explicit Commit establishes it.
- **Dismiss:** version the finding's status; do not rewrite the manuscript.

Entities, mentions and typed links are derived from findings, not an authoritative autonomous world model.
Graph navigation leads back to supporting findings and passages.

**Sources:** `src/llm/observer.c`, `src/llm/observer_job.c`, `src/llm/observer_model.c`,
`src/llm/generative_gate.c`, `src/app/record_workflow.c`, `src/app/store_records.c`,
`src/app/store_entities.c`, `tests/ui/observer_actions.scribo`, `tests/ui/store/5_entities.scribo`.

## 10. Archive, delete, remote use and recovery boundaries

**Archive** keeps a conversation and lets it be opened/continued, while setting it aside from Recall and
new observer reading. **Groups** organize chats; they are not memory scopes.
**Delete** is an explicit irreversible action, unlike ordinary append-only record versioning.
On the DB path it removes the chat and its derived memory, chat-only findings and unneeded graph data,
while preserving author-saved notes/canon with the chat link removed. Index removals that fail stay on a
durable retry list. Older backups, exports and legacy files are not erased.
The file backend deletes only the chat file.

**Remote** is configured separately and selected explicitly each session. Requests may carry the current
author input, explicit attached/selected text and permitted remote-marked history; local retrieval is
excluded. HTTPS/key validation, no redirects, visible destination indicators and saved provider markers are
transport/privacy controls, not automatic consent to export the rest of the project.
The observer remains local, and neither direction has automatic model fallback.

**Backup** runs on a worker: project-specific structured export, whole-database dump when applicable,
manuscript/file copy, manifest and checksums. The SQL snapshots are transactionally consistent; this is not
a single atomic filesystem-plus-database snapshot. Symlinks/special files are refused.
In-app restore makes a separate project copy using the structured export, whereas `tools/restore.sh`
can restore the whole database and has correspondingly stronger destructive-action restrictions.
Qdrant can be discarded and rebuilt rather than treated as backup authority.

Process-death recovery tests do not establish power-loss durability. The documented macOS drive-cache
limitation remains, as do the open audit defects.

**Sources:** `src/app/chat_workflow.c:request_chat_delete`, `chat_delete_tick`,
`src/app/store_chats.c:sdb_chat_delete`, `src/llm/semantic.c:semantic_forget_pending`,
`src/llm/remote.c`, `src/llm/chat_history.c`, `src/app/backup.c`, `src/app/backup_job.c`,
`tools/backup.sh`, `tools/restore.sh`, [GAP-004](GAP-004-interruption-matrix.md).

## 11. Defaults, history and evidence

The target is currently macOS on Apple Silicon, built from source. The older SOW still describes Linux
and raygui; the current app uses custom widgets and the README explicitly excludes Linux from the MVP.
Historical design headings and comments saying “experiment” are not sufficient to determine current routing.

`src/core/experiments.c`/`.h`, `app_start_models()` and the request stages establish what is active:
local-memory availability and the verified pipeline are default-on, **per-conversation consent remains off
until granted**, and generated T1 briefs, alternate presentations, canon-aware chat and observer conflict
remedies are separate gated paths. Canon-aware chat is not an automatic default injection of every canon
entry into creative conversation. Failed experiments remain available for reproduction, not as implicit
product behavior.

The generative model uses the pinned unquantized BF16 GGUF with the validated BF16/F32 execution policy,
not arbitrary lower-precision weights. GGUF is a container, not synonymous with quantization.
Flash attention is disabled; context, shared reserve, batching and checkpoint settings are measured runtime
choices. The embedding model is separately pinned F16; the reranker is separately pinned BF16.

The repository separates:

- mechanical correctness: headless tests, allocation faults, sanitizers, static analysis;
- backend/recovery contracts: database, interruption and backup suites;
- integration: scripted real-app UI with fake or real models;
- model quality: reach versus faithful-use measurements on frozen/held-out manuscript fixtures;
- operational evidence: manuscript-scale latency, memory, frames, backlog and restart/soak runs.

None substitutes for the others. Existing dated measurements are not new measurements from this review.
Whole-manuscript performance/AI usefulness remain active work; installation, real macOS input/accessibility
and author-pilot evidence remain product gaps. The audit's newly reproduced persistence defects also qualify
older broad “retention done” claims.

**Sources:** `README.md`, `src/core/experiments.h`, `src/app/app_lifecycle.c:app_start_models`,
[MODEL-001](MODEL-001-lfm3-runtime.md), [TESTING](TESTING.md), [LANDSCAPE](LANDSCAPE.md),
[PRODUCT-GAPS](PRODUCT-GAPS.md), [AUDIT-005](AUDIT-005-repository-audit.md).

## 12. Scope of this understanding pass

The map follows the actual public entry points, orchestration stages, store routing, background protocols
and representative tests for a complete authoring cycle, including failure and restart paths.
It does not claim that every line of first-party code or vendored model/runtime code was exhaustively audited.

This pass retained the preceding headless/analyzer results and reran the retained audit probe successfully.
It did not start the author's GUI session, change real project data, launch the real models, run database
integration or repeat historical quality/performance measurements. Those remain empirical validation work,
not missing distinctions in the architecture above.
