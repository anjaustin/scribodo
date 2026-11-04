# Story Workbench — MVP Design Specification

**Status:** Draft v0.2  
**Product:** Local-first, AI-assisted book-writing workbench  
**Primary goal:** Let an author brainstorm in chat, develop prose in documents, and inspect automatically extracted narrative observations without interrupting the creative conversation.

## 1. Product principles

1. **Conversation-first.** The author can begin with an empty chat; no outline, project structure, or writing mode is required.
2. **One workspace.** Brainstorming, rough drafting, and editing happen in the same session. A document can be opened, created, or attached at any time.
3. **Separation of roles.** The creative assistant responds to the author. A separate observer processes the conversation and documents but never speaks into the creative chat on its own.
4. **Author authority.** Observer findings are suggestions. Only explicit author actions change a document or establish canon.
5. **Local by default.** Project data and application services remain on the user's machine. All core workflows work without an internet connection using a configured local model. Remote frontier models are optional and require an explicit provider configuration and selection.

## 2. MVP user workflow

1. Create or open a local project.
2. Start a chat with the creative assistant and brainstorm freely.
3. Create, upload, or select a document in the project; continue the same chat to develop rough prose.
4. View the document beside chat, edit it directly, or ask the assistant to propose changes.
5. Review proposed edits and accept or reject them; accepted edits become a new saved document revision.
6. See observer findings in a separate panel as chat and document content changes.
7. Select a finding to **Discuss in chat**, **Save as note**, **Commit to canon**, or **Dismiss**. A selected finding enters creative-chat context only when the author chooses **Discuss**.
8. Reopen the project later with chat history, documents, notes, canon, and findings intact.

## 3. MVP interface

A VS Code-style native desktop workbench, implemented with raylib + raygui + Clay, with resizable panes:

- **Explorer:** local project, chats, documents, notes, and canon entries.
- **Main area:** tabbed chat and document editor; supports side-by-side chat and document viewing.
- **Observer sidebar:** list of extracted findings and their source references, with author-controlled actions.
- **Model selector:** local model by default; optional configured remote provider.

The observer must not insert messages into the conversation or alter active assistant instructions automatically.

## 4. Functional requirements

### 4.1 Project and document management

- Create/open a project stored in a user-selected local directory.
- Create, import, rename, open, edit, and save Markdown (`.md`) and plain-text (`.txt`) documents.
- Keep document revisions so accepted AI changes can be inspected or reversed.
- Allow a chat to reference one or more existing project documents without making copies.
- Show which documents are attached to the current chat.

**Out of scope:** DOCX/PDF import or export, page layout, print formatting, and multi-user collaboration.

### 4.2 Creative chat

- Persistent chat history per project; start, rename, and reopen conversations.
- Streaming answers from a locally configured model.
- Attach/select project documents as contextual input to the conversation.
- Ask for brainstorming, scene development, critique, or prose drafting in ordinary language; no mandatory modes.
- Generate a proposed document edit or replacement, displayed for review before application.
- Never modify manuscript files without explicit approval.

### 4.3 Parallel observer

- Operate independently from creative chat after new chat messages and saved document revisions.
- Extract a **bounded MVP set** of findings: characters/entities, events, relationships, unresolved questions, and potential continuity conflicts.
- Store each finding with type, short description, evidence/source span, creation time, and status.
- Compare newly extracted findings with existing project notes and committed canon; surface potential overlaps or conflicts with supporting references.
- Deduplicate findings and process updates asynchronously so chat remains responsive.
- Present findings only in the observer sidebar; never inject them into the creative assistant's context without the author's explicit action.
- Support actions: **Discuss**, **Save as note**, **Commit to canon**, **Dismiss**.

**MVP graph scope:** Store entities and typed links locally and show a simple linked-entity view or navigable relationship list. Advanced community detection, thematic clustering, structural-gap analysis, and an InfraNodus integration are deferred.

### 4.4 Notes and canon

- Notes are editable, non-authoritative project records.
- Canon entries are explicit, author-approved assertions with a source link and revision history.
- A finding does not become canon merely by appearing in a chat, document, note, or graph.
- Canon may be revised or superseded by the author; prior versions remain traceable.
- Potential conflicts are warnings, not automatic corrections.

### 4.5 Inference

- **Primary local inference:** use LiquidAI `LFM3-1.7B` (post-trained text checkpoint) with original BF16 weights and BF16 computation where supported. Quantized checkpoints, weight conversion to lower precision, and quantized inference are prohibited.
- **Model runtime:** encapsulate the BF16 model behind a replaceable local inference adapter. Choose a runtime supported on the actual target hardware and verify precision end-to-end before accepting it.
- **Context:** the model advertises up to 256K tokens, but configure a lower practical context limit based on measured memory/performance when necessary.
- **Observer inference:** may share the LFM3-1.7B runtime, using a separate request queue and task-specific prompting; it must not starve interactive chat.
- **Optional frontier inference:** support at least one remote API-compatible provider through a separate adapter, disabled until configured by the user.
- The creative assistant defaults to the local LFM3-1.7B BF16 model. The observer can use the same local runtime. Optional frontier-provider selection does not change the required BF16-only policy for local inference.
- Remote model requests must indicate when project content will leave the device; no automatic fallback from local to remote.
- Keep API credentials in OS-provided secure storage, not in project files.

### 4.6 Semantic retrieval

- Use **Qdrant** as the local semantic vector store for indexed chat messages, document revisions, notes, and canon entries.
- Use a **separate locally executed embedding model** to produce vectors; LFM3-1.7B is the generative model and is not assumed to provide suitable embeddings.
- Select and pin the embedding model, embedding dimensions, and distance metric during implementation; do not silently mix vectors from different models.
- Chunk and index source text with stable MariaDB IDs, revision IDs, chunk boundaries, and status/authority metadata so retrieval can return precise source references.
- Treat Qdrant as a rebuildable derived index, **not** the source of truth. Deletes, superseded revisions, and canon status updates must be reflected in the index.
- Retrieval must distinguish committed canon from speculative or draft material. Retrieved material never becomes canon automatically.
- Run Qdrant locally; no hosted Qdrant service is required.

## 5. Minimal data model

| Object | Required fields |
|---|---|
| Project | ID, name, local path, timestamps |
| Conversation | ID, project ID, title, timestamps |
| Message | ID, conversation ID, role, content, timestamp, referenced document IDs |
| Document | ID, project ID, path, current revision ID |
| Revision | ID, document ID, content/hash, timestamp, origin |
| Finding | ID, project ID, type, description, source references, status, timestamp |
| Entity / Link | IDs, names/types, relationship type, source references |
| Note | ID, project ID, content, source references |
| Canon entry | ID, project ID, assertion, source references, status, revision history |

**Finding status:** `new`, `saved`, `committed`, `dismissed`.  
**Source reference:** document revision and text range, or conversation message and text range. Preserve stable IDs even when the underlying text changes.

**Structured authority:** MariaDB stores project records, conversations, messages, document/revision metadata, findings, notes, canon, entities, typed links, source references, and background job state. Markdown/plain-text manuscript files remain human-readable in the selected project directory. Qdrant points reference MariaDB/source IDs, and can be regenerated from authoritative content.

## 6. Implementation stack (MVP decisions)

| Component | Choice | Constraint |
|---|---|---|
| Native frontend | **raylib + raygui + Clay** | Native desktop workbench; no Tauri, React, or embedded browser dependency |
| Document editor | Native custom text-editing surface integrated with the UI stack | Markdown/plain text, selection, scroll, UTF-8 input, undo/redo, and reviewable AI diffs are MVP requirements |
| Structured database | **MariaDB** | Local-only service; migrations, foreign keys, transactions, and stable IDs |
| Semantic database | **Qdrant** | Local vector search; derived, rebuildable index with source provenance |
| Embeddings | Dedicated local embedding model (to be selected) | Stable model/dimensions; no cloud dependency |
| Primary generative model | **LiquidAI/LFM3-1.7B** | Official post-trained text checkpoint in **BF16**, no quantization |
| Inference serving | Local BF16-compatible adapter/runtime | Validate actual hardware support; do not assume one backend works identically on macOS and Linux |
| Observer | Independent local worker and job queue | Processes stored events, writes findings to MariaDB and embeddings to Qdrant |
| Optional frontier models | Explicit remote adapter(s) | Disabled unless configured and selected; clear data-exit indicator |

**Model reference:** https://huggingface.co/LiquidAI/LFM3-1.7B

**Model facts:** The model card describes 1.73B parameters and a 256K-token maximum context window. It lists BF16 support with vLLM, SGLang, and Transformers; the listed MLX variant is 4-bit and therefore **does not meet** this specification. Do not substitute quantized GGUF, FP8, NVFP4, or 4-bit MLX checkpoints. The exact BF16 execution path on Apple Silicon and Linux is a required early engineering validation, not an assumed capability.

**Expected model weight memory:** 1.73B parameters × 2 bytes/parameter is approximately **3.46 GB decimal** for BF16 weights alone, before runtime buffers, activations, and KV/cache state. Size the context window and parallel observer concurrency against actual memory measurements.

**Local deployment:** MariaDB, Qdrant, model serving, and the observer must bind only to local interfaces by default. Define explicit startup, health-check, shutdown, migration, backup, and recovery behavior. External provider access is separately configured.

**Target platforms:** macOS and Linux desktop. Windows is not required for MVP.

## 7. Nonfunctional requirements

- All MVP authoring, observation, retrieval, and persistence functions operate offline with a configured local model.
- The UI remains usable while observer jobs run; observation failures do not block writing.
- All project content is saved locally; backups must include project files plus a transactionally consistent MariaDB export. The Qdrant index may be rebuilt from source content; optionally snapshot it to speed recovery.
- No telemetry, hosted account, mandatory login, or cloud sync. MariaDB and Qdrant remain local services and must not expose unauthenticated public listeners.
- A model response or observer finding must never silently overwrite user-authored text or canon.
- Remote-provider usage is visibly identified and opt-in.

## 8. Acceptance criteria

The MVP is complete when a user can:

1. Create a project, brainstorm with a local model offline, close the app, and resume the same chat.
2. Create/import a Markdown document and work on it in the same session without changing writing modes.
3. Ask the assistant for an edit, inspect the proposed change, accept/reject it, and restore the previous revision.
4. See observer findings generated in parallel, each linked to exact source material.
5. Select a finding and explicitly send it to the creative chat without unrelated findings being injected.
6. Save a finding as a note or commit it as canon, then see future potential conflicts flagged against that canon.
7. Navigate an entity's stored relationships and open their supporting sources.
8. Complete the above without any network connection; optionally choose a configured remote model with a clear data-sharing indication.
9. Confirm the active local generative model is LFM3-1.7B with BF16 weights/inference, without quantized checkpoints.
10. Rebuild a wiped Qdrant index from MariaDB and project files; retrieved passages retain exact provenance and canon/draft distinctions.
11. Launch the native raylib + raygui + Clay workbench on macOS and Linux and validate document editing, resizing, and keyboard input.

## 9. Explicitly deferred

- Full InfraNodus feature parity or third-party plugin integration.
- Large-scale graph analytics, automated plotting, and thematic gap discovery.
- Multiple simultaneous creative agents, autonomous story generation, or automatic canon promotion.
- Rich publication layout, typesetting, DOCX/PDF pipelines, and publishing integrations.
- Cloud synchronization, web/mobile clients, collaborative editing, and hosted accounts.
- Complex timeline visualization, character-voice scoring, or global manuscript quality dashboards.

## 10. Build order

1. **Feasibility gate:** verify native raylib + raygui + Clay text editor/UTF-8 workflows; validate LFM3-1.7B BF16 runtime on the target Mac/Linux hardware; validate local MariaDB and Qdrant operation.
2. **Foundation:** native workbench layout, projects, Markdown documents, MariaDB schema/migrations, revision history.
3. **Creative workflow:** persistent chat, BF16 local inference adapter, document context, reviewed AI edits.
4. **Semantic and observer pipeline:** local embeddings, Qdrant indexing/retrieval, asynchronous extraction, provenance, findings panel, notes/canon actions.
5. **Connections and verification:** entity/link navigation, canon comparisons, optional remote adapter, offline integration tests, backup/recovery, acceptance tests.

**MVP boundary:** A local storytelling workbench in which chat and documents coexist, observation happens independently, and the author controls every consequential change.
