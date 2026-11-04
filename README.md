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

**Targets:** macOS and Linux desktop. Windows is not in scope for the MVP.

### Model policy

The local model must run with BF16 weights and BF16 compute. Quantized checkpoints (GGUF, FP8, NVFP4, 4-bit MLX, and so on) and lower-precision conversions are prohibited. BF16 weights alone take about 3.46 GB, before activations and KV cache, so context length and observer concurrency are sized from measured memory. Scribodo never falls back from local to remote automatically.

### Data and provenance

- Manuscripts stay as human-readable `.md` and `.txt` files in your project directory.
- Every finding, note, and canon entry carries **source references**: a document revision or chat message, plus a text range, with stable IDs.
- Retrieval distinguishes committed canon from draft or speculative material. Nothing becomes canon automatically.
- A backup consists of the project files plus a transactionally consistent MariaDB export. The Qdrant index can be regenerated.
- There is no telemetry, no account, no login, and no cloud sync. All services bind only to local interfaces.

## Roadmap

1. **Feasibility gate.** Validate raylib, raygui, and Clay text editing with UTF-8; LFM3-1.7B BF16 on Mac and Linux hardware; and local MariaDB and Qdrant.
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

*Coming soon. Build instructions will be added once the feasibility gate is complete.*
