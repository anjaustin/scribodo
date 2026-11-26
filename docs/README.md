# Documentation map

The docs are numbered series written as the work happened, so their file
names follow history, not topic. This page groups them by what you would
come to them for. Every series keeps its old entries: a superseded design or
a failed experiment stays on record, and the newer document names what
replaced it.

## Start here

| Document | What it is |
|---|---|
| [`../README.md`](../README.md) | The product, quick start, architecture and status |
| [ARCHITECTURE](ARCHITECTURE.md) | Source-traced end-to-end workflows, authority boundaries, memory paths and failure/recovery behavior |
| [USER-GUIDE](USER-GUIDE.md) | Every feature, and how saving protects your text |
| [SERVICES](SERVICES.md) | The project store: native or Docker, versions, migration, durability |
| [CONFIGURATION](CONFIGURATION.md) | Every setting the app reads, grouped |
| [`../CONTRIBUTING.md`](../CONTRIBUTING.md) | The gates, the working rules, and the docs and evidence conventions |
| [LANDSCAPE](LANDSCAPE.md) | Every capability and gap in one table, with its evidence |
| [SOW](SOW.md) | The MVP specification (v0.2, first titled "Story Workbench") |
| [MEMORY-015](MEMORY-015-effective-memory-contract.md) | The memory contract: nothing written is forgotten, the record is never corrupted, model errors are measured and disclosed |

## Status and plans

| Document | What it is |
|---|---|
| [COMPOSITION-001](COMPOSITION-001-plan.md) | **Active P0:** lightweight component adoption plan, decision gates and cross-session work ledger |
| [COMPOSITION-001 revision identity](COMPOSITION-001-revision-identity.md) | Active C02 boundary map, legacy-ownership constraints and recovery evidence |
| [REDTEAM-027](REDTEAM-027-revision-identity-integration.md) | C02 production identity/recovery review, remediations and acceptance limits |
| [REDTEAM-026](REDTEAM-026-revision-identity-prototype.md) | C02 prototype confinement, test-oracle and red-evidence review |
| [REDTEAM-025](REDTEAM-025-composition-save-state.md) | Adversarial C01 save-state review, remediations and verification boundaries |
| [PRODUCT-GAPS](PRODUCT-GAPS.md) | The five product-readiness gaps (GAP-001 to 005) and the working order |
| [TECHNICAL-DEBT](TECHNICAL-DEBT.md) | The debt register, including the [items deferred after AUDIT-004](TECHNICAL-DEBT.md#deferred-after-audit-004-weighted-2026-11-16), weighted |
| [ACCEPTANCE](ACCEPTANCE.md) | The MVP acceptance run on the development Mac (SOW §8) |

## Design

| Document | What it is |
|---|---|
| [STORE-001](STORE-001-design.md) | MariaDB as the structured authority |
| [OBSERVER-001](OBSERVER-001-design.md) | The background observer: design and validation |
| [MEMORY-002](MEMORY-002-mvp-design.md) | The MariaDB and Qdrant memory design, with its [finite design model](models/memory_mvp_model.py) |

## Models

| Document | What it is |
|---|---|
| [MODEL-001](MODEL-001-lfm3-runtime.md) | LFM3-1.7B BF16 under llama-server on macOS: settings, limits and licence |
| [MODEL-002](MODEL-002-qwen3-reranker.md) | Qwen3-Reranker-0.6B as the verified-answer pipeline's selector |

## The memory investigation (MEMORY-001 to 022)

Read in order, this series is the history of the P0 memory work. The
outcome is stated where it was a decision.

| Range | Topic |
|---|---|
| [001](MEMORY-001-long-lived-context.md) | The problem: long-lived memory with bounded resources |
| [002](MEMORY-002-mvp-design.md), [003](MEMORY-003-implementation-progress.md) | The memory MVP design and its build |
| [004](MEMORY-004-history-windows.md), [005](MEMORY-005-async-history.md) | Bounded history windows; asynchronous history and scale diagnostics |
| [006](MEMORY-006-retrieval-pipeline.md), [007](MEMORY-007-saved-revision-authority.md), [008](MEMORY-008-saved-document-retrieval.md) | Durable chat ingestion, saved-revision authority, saved-document retrieval |
| [009](MEMORY-009-resource-and-brief-experiment.md), [010](MEMORY-010-quality-bedrock.md) | Resource bounds and the brief experiment; a root-cause investigation of quality |
| [011](MEMORY-011-native-continuity-dev.md) | Native continuity: **no-go** |
| [012](MEMORY-012-causal-continuity.md) | Causal continuity investigation |
| [013](MEMORY-013-explicit-operations-dev.md) | Explicit Recall/Draft operations: mixed gains, screen **failed** |
| [014](MEMORY-014-canon-first-continuity.md) | Canon-first continuity |
| [015](MEMORY-015-effective-memory-contract.md) | **The contract and the P0 plan**, which everything after it follows |
| [016](MEMORY-016-recall-view.md), [017](MEMORY-017-reach-harness.md), [018](MEMORY-018-selection-ranking.md), [019](MEMORY-019-manuscript-baseline.md) | Slices S1 to S4: the recall view, the reach harness, selection ranking, the manuscript-scale baseline |
| [020](MEMORY-020-canon-aware-chat.md) | Canon-aware chat |
| [021](MEMORY-021-real-manuscript.md), [022](MEMORY-022-product-remediation.md) | A real manuscript through the memory paths; the product remediation it led to |

## Testing and evidence

| Document | What it is |
|---|---|
| [TESTING](TESTING.md) | The regression suites, the reach measurement, trusted acceptance, the UI tests and what they cannot cover |
| [GAP-004 matrix](GAP-004-interruption-matrix.md) | The crash and interruption matrix: every kill point and what it checks |
| [EVIDENCE](EVIDENCE.md) | The local evidence archive: what is kept, where, and what was lost |

## Reviews

**Audits** look across the whole repository after a cycle of work. Each
lists its findings and how they were remediated.

- [AUDIT-001](AUDIT-001-repository-audit.md): the first repository audit
- [AUDIT-002](AUDIT-002-repository-audit.md): after the memory and diagnostics work
- [AUDIT-003](AUDIT-003-repository-audit.md): after the verified-pipeline cycle
- [AUDIT-004](AUDIT-004-repository-audit.md): after the native-services and observer cycles
- [AUDIT-005](AUDIT-005-repository-audit.md): targeted persistence review, reproducible chat/history defects, and dependency follow-up (open)

**Red-team reviews** attack one feature or change adversarially.
No REDTEAM-002 exists.

| Review | Subject |
|---|---|
| [001](REDTEAM-001-editor-spike.md) | The editor feasibility spike |
| [003](REDTEAM-003-chat.md), [004](REDTEAM-004-edits.md) | Chat with the local model; reviewed AI edits |
| [005](REDTEAM-005-observer.md) | The observer, notes and canon |
| [006](REDTEAM-006-projects.md), [007](REDTEAM-007-attachments.md) | Projects and documents; chat attachments and rename |
| [008](REDTEAM-008-store.md), [009](REDTEAM-009-graph-backup-remote.md) | The project store; the entity graph, backup and recovery, the remote model |
| [010](REDTEAM-010-persistence.md), [011](REDTEAM-011-legacy-fork.md), [012](REDTEAM-012-marker-snapshots.md), [013](REDTEAM-013-failure-paths.md) | Persistence, legacy import and fork recovery, marker snapshots, failure paths |
| [014](REDTEAM-014-memory-foundation.md), [015](REDTEAM-015-history-windows.md) | The bounded-history foundation and history windows |
| [016](REDTEAM-016-real-manuscript.md) | The real-manuscript changes |
| [017](REDTEAM-017-repository-audit.md), [019](REDTEAM-019-cycle-tooling.md) | The changes made in remediating the audits, and the tooling for each work cycle |
| [018](REDTEAM-018-memory-contracts.md) | Memory remediation contracts and failure paths |
| [020](REDTEAM-020-selection-tooling.md), [021](REDTEAM-021-verified-pipeline.md) | The selection-measurement tooling; the verified-answer pipeline |
| [022](REDTEAM-022-end-to-end.md) | The whole product, end to end |
| [023](REDTEAM-023-native-services-and-batch.md) | The native services and the micro-batch |
| [024](REDTEAM-024-chat-lifecycle.md) | Archive, groups and delete for chats |
| [025](REDTEAM-025-composition-save-state.md), [026](REDTEAM-026-revision-identity-prototype.md), [027](REDTEAM-027-revision-identity-integration.md) | Lightweight composition: save-state guards, identity prototype and production namespace recovery |

## Outside `docs/`

- [`../journal/`](../journal/): the working journals behind each document,
  with step-by-step measurements and the commands that produced them. The
  file name prefix gives the track: `answer-` (the verified pipeline),
  `memory-` and `memory_`, `manuscript-`, `observer-`, `store-`, `gap003-`,
  `gap004-`, `technical-debt-` and `debt-`, and `audit004-`. A document's
  figures cite the journal entry they came from.
- [`../research_notes/`](../research_notes/) and [`../reports/`](../reports/):
  a source-pinned study of an outside long-context system, with its
  provenance note. It tests MEMORY-001's early hypotheses against that
  system; it is research, not a Scribodo design.
