# Scribodo

A local-first, AI-assisted workbench for writing books.

Scribodo lets an author brainstorm in chat, develop prose in documents, and review narrative observations that are extracted automatically, without interrupting the creative conversation.

> **Status:** Pre-alpha. We are working toward the MVP described in [`docs/SOW.md`](docs/SOW.md) (spec v0.2, originally titled "Story Workbench").

---

## Principles

1. **Conversation-first.** You can start with an empty chat. No outline, project structure, or writing mode is required.
2. **One workspace.** Brainstorming, rough drafting, and editing happen in the same session. You can open, create, or attach a document at any time.
3. **Separation of roles.** The *creative assistant* talks to you. A separate *observer* reads the conversation and documents in the background. It never speaks into the chat on its own.
4. **Author authority.** Observer findings are suggestions. Documents change and canon is established only through explicit author actions.
5. **Local by default.** Data and services stay on your machine. Every core workflow works offline with a local model. Remote frontier models are optional, opt-in, and clearly labeled when in use.

## How it works

```
┌───────────────┬──────────────────────────────────┬──────────────────┐
│  Explorer     │  Chat  │  Document editor        │  Observer        │
│               │                                  │                  │
│  • Projects   │  Brainstorm, draft, and request  │  Findings:       │
│  • Chats      │  edits in plain language.        │  • Entities      │
│  • Documents  │                                  │  • Events        │
│  • Notes      │  Proposed edits are shown as     │  • Relationships │
│  • Canon      │  diffs to accept or reject.      │  • Open questions│
│               │                                  │  • Continuity ⚠  │
│               │                                  │                  │
│               │                                  │  [Discuss] [Note]│
│               │                                  │  [Canon] [Dismiss]│
└───────────────┴──────────────────────────────────┴──────────────────┘
                         Model: LFM3-1.7B (local, BF16) ▾
```

1. Create or open a local project.
2. Brainstorm with the creative assistant.
3. Create, import, or attach Markdown or plain-text documents, then keep drafting in the same chat.
4. Edit documents directly, or ask the assistant for changes. Proposed edits are reviewed before they're applied, and each accepted edit becomes a new revision.
5. Watch observer findings appear in the sidebar as the work evolves. Each finding links to its exact source.
6. Act on a finding:
   - **Discuss** sends it to the chat. This is the only way a finding enters the assistant's context.
   - **Save as note** keeps it as an editable, non-authoritative record.
   - **Commit to canon** makes it an author-approved assertion with source and revision history.
   - **Dismiss** removes it from the sidebar.
7. Close the project and come back later. Chats, documents, notes, canon, and findings are all preserved.

## Architecture

| Component | Choice | Notes |
|---|---|---|
| Native UI | **raylib + raygui + Clay** | VS Code-style desktop workbench with resizable panes. No browser, Tauri, or React. |
| Document editor | Custom native text surface | UTF-8 input, selection, scrolling, undo/redo, and reviewable AI diffs. |
| Structured store | **MariaDB** (local) | Source of truth for project records, chats, revisions, findings, notes, canon, entities and links, and job state. |
| Semantic index | **Qdrant** (local) | Derived and rebuildable. Every point references MariaDB and source IDs. |
| Embeddings | **Qwen3-Embedding-0.6B** (local, f16 GGUF) | Model, dimensions, and metric are pinned by hash. Vectors from different models are never mixed. |
| Generative model | **[LiquidAI/LFM3-1.7B](https://huggingface.co/LiquidAI/LFM3-1.7B)** | Official post-trained checkpoint, **BF16 only**. |
| Inference | Replaceable local adapter | Runtime is validated on each target platform. |
| Observer | Independent worker and job queue | Shares the runtime through a separate queue and never starves interactive chat. |
| Remote models | Optional adapter(s) | Off until configured. The API key is read from a private file named in `remote.json`, and a data-exit indicator is shown. |

**Target:** macOS on Apple Silicon. The MVP is built and validated on an Apple M4 (16 GB). Linux and Windows are out of scope for the MVP.

### Model policy

The local model must run with BF16 weights and BF16 compute. Quantized checkpoints (GGUF, FP8, NVFP4, 4-bit MLX, and so on) and lower-precision conversions are prohibited. BF16 weights alone take about 3.46 GB, before activations and KV cache, so context length and observer concurrency are sized from measured memory. Scribodo never falls back from local to remote automatically.

**An optional remote model.** You can add one OpenAI-compatible chat-completions provider. It's off unless configured and selected:

1. Put the API key in a private file: `chmod 600 ~/.config/scribodo-key`.
2. Write `~/Library/Application Support/Scribodo/remote.json`:
   ```json
   { "name": "Frontier", "base_url": "https://api.example.com/v1",
     "model": "model-name", "api_key_file": "/Users/you/.config/scribodo-key",
     "context_tokens": 32000 }
   ```
3. Click the model in the toolbar and choose it.

**How a remote model is kept in check:**
- **Selection:** every launch starts local, and only your choice in the toolbar switches.
- **Indicator:** while the remote model is selected, the toolbar (in amber) and the chat header say where chat messages, attached documents and edit passages go.
- **No fallback:** if the remote fails, you get its error. Scribodo never switches between local and remote on its own.
- **The observer** always runs locally.
- **Transport:** HTTPS is required, and redirects aren't followed. The key is only read from a private file and is sent only to the configured host.

### Data and provenance

- Manuscripts stay as human-readable `.md` and `.txt` files in your project directory.
- Every finding, note, and canon entry carries **source references**: a document revision or chat message, plus a text range, with stable IDs.
- Retrieval distinguishes committed canon from draft or speculative material. Nothing becomes canon automatically.
- A backup consists of the project files plus a transactionally consistent MariaDB export. The Qdrant index can be regenerated.
- Backups accept only regular files and directories: symlinks and special files are refused explicitly, never silently followed. Every backup is checksum-verified before success is reported; failed copies are removed.
- There is no telemetry, no account, no login, and no cloud sync. All services bind only to local interfaces.

## Roadmap

1. **Feasibility gate.** Validate raylib, raygui, and Clay text editing with UTF-8; LFM3-1.7B BF16 on this Mac ([done](docs/MODEL-001-lfm3-runtime.md)); and local MariaDB and Qdrant.
2. **Foundation.** Workbench layout, projects, Markdown documents, MariaDB schema and migrations, revision history.
3. **Creative workflow.** Persistent chat, the BF16 inference adapter, document context, reviewed AI edits.
4. **Semantic and observer pipeline.** Embeddings, Qdrant indexing and retrieval, async extraction, provenance, the findings panel, and note and canon actions.
5. **Connections and verification.** Entity and link navigation, canon conflict checks, the optional remote adapter, and backup and recovery (all done); offline integration tests and acceptance tests.

The full acceptance criteria are in [`docs/SOW.md` §8](docs/SOW.md#8-acceptance-criteria). The MVP acceptance run on this Mac is in [`docs/ACCEPTANCE.md`](docs/ACCEPTANCE.md).
Architectural and maintenance follow-up is tracked in [`docs/TECHNICAL-DEBT.md`](docs/TECHNICAL-DEBT.md).
The five remaining product-readiness gaps are tracked in [`docs/PRODUCT-GAPS.md`](docs/PRODUCT-GAPS.md).
**Current P0:** memory. The goal is infinite *effective* memory for the local
model, not a larger context window; the contract and ordered slices are in
[`MEMORY-015`](docs/MEMORY-015-effective-memory-contract.md). The investigation
began with [`MEMORY-001`](docs/MEMORY-001-long-lived-context.md), and the
bounded-history foundation is tracked in
[`MEMORY-003`](docs/MEMORY-003-implementation-progress.md).

## Out of scope for the MVP

- DOCX and PDF import or export, page layout, typesetting, publishing integrations
- Cloud sync, web or mobile clients, collaborative editing, hosted accounts
- Multiple simultaneous creative agents, autonomous story generation, automatic canon promotion
- Advanced graph analytics, InfraNodus integration, timeline visualization, quality dashboards

## Building

Requirements: a C11 compiler (Apple clang on macOS) and `make`. Every dependency is vendored as a pinned git submodule, and raylib is compiled from source.

```sh
git clone --recursive https://github.com/anjaustin/scribodo.git
cd scribodo
make llama  # build the pinned llama-server (third_party/llama.cpp; a few minutes)
make model  # download LFM3-1.7B BF16 (3.5 GB, SHA-256 checked; reuses a Hugging Face cache copy)
make run    # launch with assets/sample/sample.md
./build/scribodo path/to/chapter.md
```

For a smaller first checkout, fetch only the pinned revisions with
`git clone --recurse-submodules --shallow-submodules --depth 1 https://github.com/anjaustin/scribodo.git`.
The full dependency history is not needed to build: `.git/modules` is about
940 MB, mostly raylib and llama.cpp.

On first launch the app checks the model file against its pinned hash (once), then starts `llama-server` in the background. The toolbar shows the model's status, which turns green when it's ready. The server listens only on a private Unix socket, uses a fresh API key each launch, and stops when the app quits.

**The project store:** chats, notes, canon, findings, History and the observer's progress live in a local MariaDB (SOW §5), run in Docker. `tools/services.sh up` starts it (MariaDB 11.4 and Qdrant, localhost only, with generated credentials). The app starts it too if Docker is running.
- The first time a project is opened with the store up, its `.scribodo/` files are moved in. The old files are kept in `.scribodo/legacy-…`.
- Documents stay plain files in the folder, and a rename keeps each document's id, so History and sources follow it.
- Failed notes/history reads stop migration without switching authority. Making a copied folder its own project uses a private snapshot: retries do not duplicate chats or mix in later changes from the original.
- If the store is down, documents stay editable; notes, canon, chats and History pause until it's back. A project not yet moved keeps using its files.
- Set `SCRIBODO_STORE=off` to run without it.

**Experimental local conversation memory:** launch with
`SCRIBODO_MEMORY_EXPERIMENT=1`, open a stored chat, and right-click it to explicitly
enable/disable/rebuild memory. This retrieves older chat and currently attached
saved local revision excerpts, never silently exports them to a remote provider,
and shows read-only as-of **Sources**. Generated T1 briefs are a second opt-in
behind `SCRIBODO_MEMORY_BRIEFS=1`: locally generated, source-linked and never
canon. Everything here is default-off, and unsaved buffers are foreground-only.
The current slice is [MEMORY-009](docs/MEMORY-009-resource-and-brief-experiment.md);
the direction after the continuity experiments is
[MEMORY-014](docs/MEMORY-014-canon-first-continuity.md).

**Projects and documents:** a project is a folder of `.md` and `.txt` files.
- On launch, Scribodo reopens the project and document you last had open. The first time, it shows a welcome screen: **Open folder…**, **New project…**, **Open the sample**, and your recent projects.
- **⌘O** or **Open…** opens another folder. Dropping a folder onto the window works too.
- **+ New document** (⌘N) and **Import…** (copies files in, never overwriting) are in the Explorer.
- Right-click a document to **Rename** it or **Reveal it in Finder**. A rename keeps its History, and notes, canon and findings that quote it still open their source.

**Using it:**
- **Chat:** click the chat box (or press ⌘L), type, and press Enter. Shift+Enter adds a new line, and Esc returns to the document.
- **Attached documents:** the chips under the chat show which project documents the assistant sees. A new chat starts with the open document attached; **+ Attach** adds others, and × removes one. Documents are read when you send (the open one including unsaved edits), never copied. Each message remembers what it was sent with, and reopening a chat restores its attachments.
- **Rename a chat:** right-click it in the Explorer.
- **Copy:** under any reply, puts its text on the clipboard for pasting into the document.
- **Saving chats:** each message is saved to the local MariaDB project store, and past chats reopen from the Explorer. Before a project is moved into the store—or when running explicitly with `SCRIBODO_STORE=off`—the file backend uses `.scribodo/chats/` instead.

**Reviewed AI edits** (the assistant never changes your document by itself):
- **Ask for an edit:** select a passage and press ⌘K, then describe the change in the chat ("tighten this", "make Dvořák colder") and press Enter. Esc cancels.
- **Review:** the proposal opens as a word-level diff at the bottom of the document, with deletions struck through in red and insertions in green, and the target passage is highlighted. Nothing changes until you choose.
- **Accept** (or ⌘↩ in the document) applies it as one undoable step and records revisions from before and after. If the "before" revision can't be written, nothing is applied. **Reject** discards it. Saving the file is still ⌘S.
- **Warnings:** the review warns when a proposal is much shorter or longer than the passage, or the reply was cut off. A reply you stop part-way is never proposed.
- **Stale proposals:** the passage is tracked through your edits (and undo/redo). Text you add or remove around it moves it. If you change the passage itself, or type at an insertion point, Accept refuses rather than overwrite your newer text.
- **Apply to document**, under any chat reply, proposes that reply for the current selection (or inserts it at the cursor), through the same review. A rewrite from ⌘K needs the passage selected.
- **History** in the Explorer lists the document's 100 newest revisions (each save, AI edit and restore) from the project store; the Explorer scrolls. Clicking one restores it, undoably, after first recording the current text. The file backend keeps equivalent revisions under `.scribodo/revisions/`.

**Notes and canon** (kept in the local project store; the file backend uses `.scribodo/notes.jsonl` and `canon.jsonl`):
- **Save as note:** select a passage and press ⌘⇧N, or click **Note** under a chat reply. It's saved at once with its source; edit it in the panel under the document (⌘S saves, and closing saves too). **+ New note** in the Explorer starts a blank one.
- **Commit to canon:** select the passage that establishes a fact and press ⌘⇧C, click **Canon** under a reply, or press **To canon** on a note. It opens as a draft: state the fact, then press **Commit**. Nothing is canon until you do, and uncommitted text needs a second Close to discard.
- **Revise, Retract, Recommit:** each stores a new version. Every earlier version stays listed under the entry, and clicking one loads its wording.
- **Sources:** each entry records where it came from: the document, an immutable snapshot of its text in History, the range and the quote, or the chat reply. Clicking the source line selects the passage again, even if it has moved.

**Recall** (the **Recall** button): type a question or a few words and press Enter. The panel lists, exactly as stored, every committed canon entry whose current wording contains your terms, then verified excerpts of your original chat messages and saved document text, best match first. Above the rows it says what was searched: how many canon entries, how many conversations had memory on, off, or still indexing, and how many candidates were rejected because their source had changed. No model is involved: matching is plain text, retracted canon is excluded, other projects are never searched, and "nothing matched" means nothing matched in what was searched, not that the book never says it. Click a canon row to open the entry; click an excerpt to select its source passage. Memory excerpts need the project store and a conversation whose memory you enabled. See [`MEMORY-015`](docs/MEMORY-015-effective-memory-contract.md).

**The observer** (the **Observer** button; the count shows new findings):
- It reads what you've **saved**: documents on ⌘S and chat messages as they finish. It lists characters, places and objects, events, relationships, open questions, and **possible conflicts** with committed canon. Each finding quotes its exact source.
- It never writes into the chat, your documents, notes or canon. It pauses whenever chat is active, so replies stay fast.
- Select a finding to act on it:
  - **Discuss** puts it in the message box for you to edit and send.
  - **Save as note** stores it as a note.
  - **Commit to canon** opens a canon draft; nothing is canon until you press Commit.
  - **Dismiss** hides it.
  - **Source** selects the passage.
- Findings are kept in the local project store. The file backend uses `.scribodo/findings.jsonl`. See [`docs/OBSERVER-001-design.md`](docs/OBSERVER-001-design.md).

```sh
make sanitize  # run the same tests under AddressSanitizer + UBSan
make tsan      # the threaded code (chat, observer, supervisor) under ThreadSanitizer
make dbtest    # the store against the local MariaDB (tools/services.sh up), in a throwaway database
make historybench # bounded DB history workload: bytes/rows and diagnostic percentiles
make historysoak  # 500 samples/case; short repeated workload, not hours-long soak/SLA
make uitest-db # the app on the store: move a project in, resume after a restart, entities, a copied folder, database down
make backuptest # back up, lose the folder, database and index, restore, resume
make remotetest # the optional remote model, against a stand-in provider on 127.0.0.1
make offlinetest # acceptance 8: the store suite and the real models in a sandbox with no network but this Mac
make uitest    # scripted UI tests: the real app in a hidden window (needs an awake display)
make llmtest   # end-to-end against the real model: supervisor, streaming, auth, crash recovery
make uitest-real  # the real model through the real UI
```

Every test runs under `tools/watchdog.sh`, so a hang is killed with its stack saved to `build/hangs/` instead of stalling the run.

See [`docs/TESTING.md`](docs/TESTING.md) for how the UI tests work and what they can't cover.

You can also drop a `.md` or `.txt` file onto the window to open it. Press ⌘S to save.

**How saving protects your text:**
- Saves are atomic, follow symlinks to the real file, and keep the file's permissions, BOM and line endings.
- If another program changed the file since you opened it, Scribodo leaves that version in place and writes yours to a sibling `… (Scribodo conflict <date> <time>).md`. Saving again within 30 seconds, without further edits, overwrites the disk version.
- Files that are write-protected or contain invalid UTF-8 open read-only.

See [`docs/REDTEAM-001-editor-spike.md`](docs/REDTEAM-001-editor-spike.md) for the review behind these behaviors.

By default the build targets the host CPU (`-mcpu=native`, which includes NEON on Apple Silicon). For a portable binary, run `make ARCH_FLAGS=`.

### Environment variables

Every setting has a working default. These override where Scribodo finds its
services, models and configuration files; the test-harness variables are listed
in [`docs/TESTING.md`](docs/TESTING.md).

| Variable | Default | Effect |
|---|---|---|
| `SCRIBODO_STORE` | on | `off` runs without the MariaDB project store, using the `.scribodo/` file backend only. |
| `SCRIBODO_DB_PORT` | `33061` | MariaDB port on 127.0.0.1. |
| `SCRIBODO_QDRANT_PORT` | `6333` | Qdrant port on 127.0.0.1. |
| `SCRIBODO_SECRETS` | `~/Library/Application Support/Scribodo/services` | Folder holding the `db_password` and `qdrant_api_key` files that `tools/services.sh` generates. |
| `SCRIBODO_LLAMA_SERVER` | `llama/bin/llama-server` beside the app binary | The pinned `llama-server` executable. |
| `SCRIBODO_MODEL` | a Hugging Face cache copy, else `~/Library/Application Support/Scribodo/models/LFM3-1.7B-BF16.gguf` | Generative model file. The pinned BF16 hash is still enforced. |
| `SCRIBODO_EMBED_MODEL` | `~/Library/Application Support/Scribodo/models/Qwen3-Embedding-0.6B-f16.gguf` | Embedding model file. The pinned hash is still enforced. |
| `SCRIBODO_MEM_AVAILABLE_MB` | measured from the OS | Overrides available memory when sizing the model context window. |
| `SCRIBODO_REMOTE_CONFIG` | `~/Library/Application Support/Scribodo/remote.json` | Optional remote provider configuration. |
| `SCRIBODO_MODEL_SETTINGS` | `~/Library/Application Support/Scribodo/model-settings.json` | Sampling and context settings saved from the toolbar. |
| `SCRIBODO_PROFILE` | `~/Library/Application Support/Scribodo/profile.json` | Author profile. |
| `SCRIBODO_BACKUPS` | `~/Library/Application Support/Scribodo/backups` | Where **Back up now** writes. |
| `SCRIBODO_PROSE_FONT` | system fonts | A font file tried first for prose text. |
| `SCRIBODO_MEMORY_EXPERIMENT` | unset | `1` enables the opt-in local memory experiment described above. |
| `SCRIBODO_MEMORY_BRIEFS` | unset | `1` additionally enables generated T1 briefs. Requires the experiment flag. |
| `SCRIBODO_MEMORY_PRESENTATION` | `legacy` | `compact` or `episode` selects an experimental source layout for memory requests. Diagnostic only; requires the experiment flag and never changes what is eligible. |

### Dependencies

| Library | Version | License | Used for |
|---|---|---|---|
| [raylib](https://github.com/raysan5/raylib) | 6.2 | zlib | Window, input, rendering |
| [raygui](https://github.com/raysan5/raygui) | 5.0 | zlib | Immediate-mode controls |
| [Clay](https://github.com/nicbarker/clay) | 0.14 | zlib | Layout |
| [stb_textedit](https://github.com/nothings/stb) | 1.14 | Public domain / MIT | Cursor, selection, and navigation logic |
| [cJSON](https://github.com/DaveGamble/cJSON) | v1.7.19-6-g6d9f244 | MIT | JSON parsing and building |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | b12111-5-g5851a8e50 | MIT | Local BF16 inference server (`llama-server`) |
| [MariaDB Connector/C](https://github.com/mariadb-corporation/mariadb-connector-c) | 3.4.11 | LGPL-2.1 | MariaDB client, statically linked |

The pinned inventory and container digests are verified by `make dependency-report`.
The local services are MariaDB 11.4.13 (GPLv2) and Qdrant v1.20.0 (Apache-2.0),
pinned by digest in `docker/compose.yml`. The LFM3-1.7B weights are under the
LFM Open License v1.0, including its $10M commercial-revenue threshold; see
[MODEL-001](docs/MODEL-001-lfm3-runtime.md#license-not-legal-advice). Because
the MariaDB client is linked statically, a binary distribution must meet
LGPL-2.1 relinking requirements (part of GAP-002).

## License

[MIT](LICENSE). Vendored dependencies keep their own licenses, listed above.
