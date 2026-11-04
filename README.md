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
| Embeddings | Dedicated local embedding model (TBD) | Model, dimensions, and metric are pinned. Vectors from different models are never mixed. |
| Generative model | **[LiquidAI/LFM3-1.7B](https://huggingface.co/LiquidAI/LFM3-1.7B)** | Official post-trained checkpoint, **BF16 only**. |
| Inference | Replaceable local adapter | Runtime is validated on each target platform. |
| Observer | Independent worker and job queue | Shares the runtime through a separate queue and never starves interactive chat. |
| Remote models | Optional adapter(s) | Off until configured. Credentials live in the OS keychain, and a data-exit indicator is shown. |

**Target:** macOS on Apple Silicon. The MVP is built and validated on an Apple M4 (16 GB). Linux and Windows are out of scope for the MVP.

### Model policy

The local model must run with BF16 weights and BF16 compute. Quantized checkpoints (GGUF, FP8, NVFP4, 4-bit MLX, and so on) and lower-precision conversions are prohibited. BF16 weights alone take about 3.46 GB, before activations and KV cache, so context length and observer concurrency are sized from measured memory. Scribodo never falls back from local to remote automatically.

### Data and provenance

- Manuscripts stay as human-readable `.md` and `.txt` files in your project directory.
- Every finding, note, and canon entry carries **source references**: a document revision or chat message, plus a text range, with stable IDs.
- Retrieval distinguishes committed canon from draft or speculative material. Nothing becomes canon automatically.
- A backup consists of the project files plus a transactionally consistent MariaDB export. The Qdrant index can be regenerated.
- There is no telemetry, no account, no login, and no cloud sync. All services bind only to local interfaces.

## Roadmap

1. **Feasibility gate.** Validate raylib, raygui, and Clay text editing with UTF-8; LFM3-1.7B BF16 on this Mac ([done](docs/MODEL-001-lfm3-runtime.md)); and local MariaDB and Qdrant.
2. **Foundation.** Workbench layout, projects, Markdown documents, MariaDB schema and migrations, revision history.
3. **Creative workflow.** Persistent chat, the BF16 inference adapter, document context, reviewed AI edits.
4. **Semantic and observer pipeline.** Embeddings, Qdrant indexing and retrieval, async extraction, provenance, the findings panel, and note and canon actions.
5. **Connections and verification.** Entity and link navigation, canon conflict checks, the optional remote adapter, offline integration tests, backup and recovery, acceptance tests.

The full acceptance criteria are in [`docs/SOW.md` §8](docs/SOW.md#8-acceptance-criteria).

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

On first launch the app checks the model file against its pinned hash (once), then starts `llama-server` in the background. The toolbar shows the model's status, which turns green when it's ready. The server listens only on a private Unix socket, uses a fresh API key each launch, and stops when the app quits.

**Using it:**
- **Chat:** click the chat box (or press ⌘L), type, and press Enter. Shift+Enter adds a new line, and Esc returns to the document.
- **Include document:** when ticked, the assistant sees the open document as reference.
- **Copy:** under any reply, puts its text on the clipboard for pasting into the document.
- **Saving chats:** each message is saved to the project folder's `.scribodo/chats/`, and past chats reopen from the Explorer.

**Reviewed AI edits** (the assistant never changes your document by itself):
- **Ask for an edit:** select a passage and press ⌘K, then describe the change in the chat ("tighten this", "make Dvořák colder") and press Enter. Esc cancels.
- **Review:** the proposal opens as a word-level diff at the bottom of the document, with deletions struck through in red and insertions in green, and the target passage is highlighted. Nothing changes until you choose.
- **Accept** (or ⌘↩ in the document) applies it as one undoable step and records revisions from before and after. If the "before" revision can't be written, nothing is applied. **Reject** discards it. Saving the file is still ⌘S.
- **Warnings:** the review warns when a proposal is much shorter or longer than the passage, or the reply was cut off. A reply you stop part-way is never proposed.
- **Stale proposals:** the passage is tracked through your edits (and undo/redo). Text you add or remove around it moves it. If you change the passage itself, or type at an insertion point, Accept refuses rather than overwrite your newer text.
- **Apply to document**, under any chat reply, proposes that reply for the current selection (or inserts it at the cursor), through the same review. A rewrite from ⌘K needs the passage selected.
- **History** in the Explorer lists the document's 100 newest revisions (each save, AI edit and restore) from `.scribodo/revisions/`; the Explorer scrolls. Clicking one restores it, undoably, after first recording the current text.

**Notes and canon** (kept in the project's `.scribodo/notes.jsonl` and `canon.jsonl`):
- **Save as note:** select a passage and press ⌘⇧N, or click **Note** under a chat reply. It's saved at once with its source; edit it in the panel under the document (⌘S saves, and closing saves too). **+ New note** in the Explorer starts a blank one.
- **Commit to canon:** select the passage that establishes a fact and press ⌘⇧C, click **Canon** under a reply, or press **To canon** on a note. It opens as a draft: state the fact, then press **Commit**. Nothing is canon until you do, and uncommitted text needs a second Close to discard.
- **Revise, Retract, Recommit:** each stores a new version. Every earlier version stays listed under the entry, and clicking one loads its wording.
- **Sources:** each entry records where it came from: the document, an immutable snapshot of its text in History, the range and the quote, or the chat reply. Clicking the source line selects the passage again, even if it has moved.

```sh
make sanitize  # run the same tests under AddressSanitizer + UBSan
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

### Dependencies

| Library | Version | License | Used for |
|---|---|---|---|
| [raylib](https://github.com/raysan5/raylib) | 6.2 | zlib | Window, input, rendering |
| [raygui](https://github.com/raysan5/raygui) | 5.0 | zlib | Immediate-mode controls |
| [Clay](https://github.com/nicbarker/clay) | 0.14 | zlib | Layout |
| [stb_textedit](https://github.com/nothings/stb) | 1.14 | Public domain / MIT | Cursor, selection, and navigation logic |
