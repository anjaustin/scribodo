# Scribodo

[![CI](https://github.com/anjaustin/scribodo/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/anjaustin/scribodo/actions/workflows/ci.yml)
[![Trusted acceptance](https://github.com/anjaustin/scribodo/actions/workflows/acceptance.yml/badge.svg?branch=main)](https://github.com/anjaustin/scribodo/actions/workflows/acceptance.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A local-first, AI-assisted workbench for writing books.

Scribodo lets an author brainstorm in chat, develop prose in documents, and
review narrative observations that are extracted automatically, without
interrupting the creative conversation. Everything runs on your Mac: a local
model, a local database, no account and no telemetry.

> **Status: pre-alpha.** The MVP workflows are built and pass acceptance on
> the development Mac ([ACCEPTANCE](docs/ACCEPTANCE.md)). There is no
> installer yet: you build from source. See [Status](#status) for what is
> still open.

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

1. Create or open a local project: a folder of Markdown or plain-text files.
2. Brainstorm with the creative assistant.
3. Create, import, or attach documents, then keep drafting in the same chat.
4. Edit documents directly, or ask the assistant for changes. Proposed edits are reviewed as a diff before they're applied, and each accepted edit becomes a new revision.
5. Watch observer findings appear in the sidebar as the work evolves. Each finding links to its exact source.
6. Act on a finding: **Discuss** it in the chat, **Save as note**, **Commit to canon**, or **Dismiss** it.
7. Close the project and come back later. Chats, documents, notes, canon, and findings are all preserved.
8. Ask what the book says: **Recall** lists, with no model involved, every canon entry and source passage that matches, and conversations with memory on answer from verified excerpts of the manuscript.

The [user guide](docs/USER-GUIDE.md) covers each feature in detail.

## Quick start

**Requirements:** a Mac with Apple Silicon (built and validated on an M4 with
16 GB); the Xcode command line tools; Homebrew's `openssl@3` and `cmake`;
`python3`; and the store's two services, natively (recommended) or in Docker
Desktop.

```sh
brew install openssl@3 cmake mariadb@11.4
git clone --recursive https://github.com/anjaustin/scribodo.git
cd scribodo
make qdrant # fetch the pinned Qdrant release binary
make llama  # build the pinned llama-server (third_party/llama.cpp; a few minutes)
make model  # download LFM3-1.7B BF16 (3.5 GB, SHA-256 checked; reuses a Hugging Face cache copy)
make run    # launch with assets/sample/sample.md
./build/scribodo path/to/chapter.md
```

On first launch the app checks the model file against its pinned hash (once),
then starts `llama-server` and the store's services in the background. The
toolbar shows the model's status, which turns green when it's ready. The model
server listens only on a private Unix socket, uses a fresh API key each launch,
and stops when the app quits.

- **The other two models:** semantic memory needs the Qwen3-Embedding-0.6B f16 GGUF, and the verified-answer pipeline the Qwen3-Reranker-0.6B BF16 GGUF, both in `~/Library/Application Support/Scribodo/models/` and checked against pinned hashes. `make` does not fetch them yet (part of GAP-002); [MODEL-002](docs/MODEL-002-qwen3-reranker.md) gives the reranker's conversion and hash, and [CONFIGURATION](docs/CONFIGURATION.md#app-models-and-files) the paths. Without the reranker the app says so and answers plainly.
- **Smaller checkout:** `git clone --recurse-submodules --shallow-submodules --depth 1 https://github.com/anjaustin/scribodo.git`. Full dependency history is not needed to build ([measured](docs/REDTEAM-017-repository-audit.md#clean-shallow-checkout)).
- **Portable binary:** the build targets the host CPU (`-mcpu=native`); `make ARCH_FLAGS=` builds a portable one.
- **Without the store:** `SCRIBODO_STORE=off` runs on per-project files only.
- **Services:** native or Docker, versions, and moving a store between them are in [SERVICES](docs/SERVICES.md). Every override is in [CONFIGURATION](docs/CONFIGURATION.md).

## Architecture

| Component | Choice | Notes |
|---|---|---|
| Native UI | **raylib + raygui + Clay** | Desktop workbench with resizable panes. No browser, Tauri, or React. |
| Document editor | Custom native text surface | UTF-8 input, selection, scrolling, undo/redo, and reviewable AI diffs. |
| Structured store | **MariaDB** (local) | Source of truth for project records, chats, revisions, findings, notes, canon, entities and links, and job state. |
| Semantic index | **Qdrant** (local) | Derived and rebuildable. Every point references MariaDB and source IDs. |
| Embeddings | **Qwen3-Embedding-0.6B** (local, f16 GGUF) | Model, dimensions, and metric are pinned by hash. Vectors from different models are never mixed. |
| Reranker | **Qwen3-Reranker-0.6B** (local) | Picks the passage the verified-answer pipeline checks ([MODEL-002](docs/MODEL-002-qwen3-reranker.md)). |
| Generative model | **[LiquidAI/LFM3-1.7B](https://huggingface.co/LiquidAI/LFM3-1.7B)** | Official post-trained checkpoint, **BF16 only** ([MODEL-001](docs/MODEL-001-lfm3-runtime.md)). |
| Observer | Independent worker and job queue | Shares the runtime through a separate queue and never starves interactive chat ([OBSERVER-001](docs/OBSERVER-001-design.md)). |
| Remote models | Optional adapter | Off until configured; a data-exit indicator is shown while in use ([user guide](docs/USER-GUIDE.md#an-optional-remote-model)). |

**Model policy.** The local model runs with BF16 weights and BF16 compute.
Quantized checkpoints (GGUF, FP8, NVFP4, 4-bit MLX, and so on) and
lower-precision conversions are prohibited. BF16 weights alone take about
3.46 GB, before activations and KV cache, so context length and observer
concurrency are sized from measured memory. Scribodo never falls back from
local to remote automatically.

**Data and provenance.**
- Manuscripts stay as human-readable `.md` and `.txt` files in your project directory.
- Every finding, note, and canon entry carries **source references**: a document revision or chat message, plus a text range, with stable IDs.
- Retrieval distinguishes committed canon from draft or speculative material. Nothing becomes canon automatically.
- A backup is the project files plus a transactionally consistent MariaDB export, checksum-verified before success is reported. The Qdrant index can be regenerated.
- There is no telemetry, no account, no login, and no cloud sync. All services bind only to local interfaces.

**Target:** macOS on Apple Silicon. Linux and Windows are out of scope for the MVP.

## Status

The MVP specification is [SOW](docs/SOW.md) (v0.2, first titled "Story
Workbench"); its acceptance criteria pass on the development Mac. The
one-table view of every capability and gap, with evidence, is
[LANDSCAPE](docs/LANDSCAPE.md).

**Current priority: memory.** The goal is effective memory for a small local
model over a whole manuscript, not a larger context window. The contract is
[MEMORY-015](docs/MEMORY-015-effective-memory-contract.md): nothing the author
wrote is forgotten, the record is never corrupted, and the model's errors are
measured and disclosed.

**Open product gaps** ([PRODUCT-GAPS](docs/PRODUCT-GAPS.md)):

| Gap | Status |
|---|---|
| GAP-001 AI usefulness and accuracy, measured on held-out novels | Under way |
| GAP-002 Turnkey installation and signed releases | Not started |
| GAP-003 Whole-manuscript scale and sustained use | Under way |
| GAP-004 Crash and interruption coverage | Done |
| GAP-005 Real macOS input, accessibility and author usability | Not started |

Engineering follow-up, including the items deferred after the last audit and
their weighting, is in [TECHNICAL-DEBT](docs/TECHNICAL-DEBT.md).

**Out of scope for the MVP:** DOCX and PDF import or export, page layout and
publishing; cloud sync, web or mobile clients, collaborative editing and
hosted accounts; multiple creative agents, autonomous story generation and
automatic canon promotion; advanced graph analytics and timeline
visualisation.

## Testing

`make test` runs the unit and regression suites; it needs no services. CI runs
it with static analysis and both sanitizers on every push, and a self-hosted
Mac runs the store, UI and crash suites as trusted acceptance.

```sh
make test      # unit and regression suites, the evidence check and the observer yardstick
make analyze   # clang static analysis
make sanitize  # the tests under AddressSanitizer + UBSan
make tsan      # the threaded code (chat, observer, supervisor) under ThreadSanitizer
make dbtest    # the store against the local MariaDB, in a throwaway database
make crashtest # the interruption matrix: a process death at every crash point, then recovery checked
make uitest    # scripted UI tests: the real app in a hidden window (needs an unlocked console)
make uitest-db # the app on the store: move a project in, resume after a restart, database down
make backuptest   # back up, lose the folder, database and index, restore, resume
make servicestest # the services script, including a migration that fails and rolls back
make offlinetest  # the store suite and the real models in a sandbox with no network
make llmtest   # end-to-end against the real model
make uitest-real  # the real model through the real UI
make remotetest   # the optional remote model, against a stand-in provider on 127.0.0.1
make historybench # bounded DB history workload: bytes/rows and diagnostic percentiles
make historysoak  # 500 samples/case; a short repeated workload, not an hours-long soak
```

Every test runs under `tools/watchdog.sh`, so a hang is killed with its stack
saved to `build/hangs/` instead of stalling the run. [TESTING](docs/TESTING.md)
covers every target, how the UI tests work, and what they can't cover.

## Documentation

- [User guide](docs/USER-GUIDE.md): every feature, and how saving protects your text
- [Services](docs/SERVICES.md) and [configuration](docs/CONFIGURATION.md): the store, and every setting
- [Documentation map](docs/README.md): designs, the memory investigation, audits and red-team reviews, by topic
- [Contributing](CONTRIBUTING.md) and [security](SECURITY.md)

## Dependencies

| Library | Version | License | Used for |
|---|---|---|---|
| [raylib](https://github.com/raysan5/raylib) | 6.2 | zlib | Window, input, rendering |
| [raygui](https://github.com/raysan5/raygui) | 5.0 | zlib | Immediate-mode controls |
| [Clay](https://github.com/nicbarker/clay) | 0.14 | zlib | Layout |
| [stb_textedit](https://github.com/nothings/stb) | 1.14 | Public domain / MIT | Cursor, selection, and navigation logic |
| [cJSON](https://github.com/DaveGamble/cJSON) | v1.7.19-6-g6d9f244 | MIT | JSON parsing and building |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | b12111-5-g5851a8e50 | MIT | Local BF16 inference server (`llama-server`) |
| [MariaDB Connector/C](https://github.com/mariadb-corporation/mariadb-connector-c) | 3.4.11 | LGPL-2.1 | MariaDB client, statically linked |

Every library is a pinned git submodule; raylib is compiled from source.
`make dependency-report` verifies the pinned inventory and container digests.
The local services are MariaDB 11.4.13 (GPLv2) and Qdrant v1.20.0
(Apache-2.0), pinned by digest in `docker/compose.yml`; the native MariaDB is
checked against the same release (`tools/services.defaults`). The LFM3-1.7B
weights are under the LFM Open License v1.0, including its $10M
commercial-revenue threshold; see
[MODEL-001](docs/MODEL-001-lfm3-runtime.md#license-not-legal-advice). Because
the MariaDB client is linked statically, a binary distribution must meet
LGPL-2.1 relinking requirements (part of GAP-002).

## License

[MIT](LICENSE). Vendored dependencies keep their own licenses, listed above.
