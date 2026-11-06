# Technical debt register

This is the durable cross-session register for architectural and maintenance
work. It records debt rather than individual defects. Update the status,
evidence and session log when a remediation lands.

**Last reviewed:** 2026-11-05
**Baseline:** `a9652c7` (`Route project stores and own service startup`)
**Status:** pre-alpha; macOS on Apple Silicon is the MVP target

## Status legend

- **Open** — accepted debt, no implementation started.
- **In progress** — a migration seam exists and further call sites remain.
- **Blocked** — the next step needs infrastructure or a decision.
- **Done** — acceptance criteria are met; retain the entry as history.

## P1 — Storage policy leaks across layers

**Status:** Done
**Why now:** each structured feature otherwise repeats file/database/paused
dispatch and teaches UI/domain code about persistence topology.

### Evidence

- Storage-mode branches are spread through `main.c`, chats, records,
  revisions, observer, import and backup code.
- The same three-way dispatch is repeated in domain modules.
- `store_mode()` combines policy resolution with retrieval of a thread-local
  database connection.

### Target architecture

- Resolve a short-lived, thread-bound `ProjectStore` for one operation.
- Select file, database or paused behavior behind operations, not in callers.
- Represent UI needs as capabilities and blocking reasons rather than backend
  names.
- Keep import/export file-only entry points explicit.
- Never cache a `ProjectStore`, its `Db *`, or its mode across calls, frames or
  threads.

### Migration plan

- [x] Add an internal `ProjectStore` routing seam.
- [x] Route records list/create/append through the seam.
- [x] Route revisions through the seam.
- [x] Route chats through the seam.
- [x] Route observer state and rules through the seam.
- [x] Add capability/blocking inspection for UI and semantic/entity features.
- [x] Remove direct `store_mode()` calls from ordinary UI/domain workflows.
- [x] Leave import, recovery and backup transition orchestration until last.

### Acceptance criteria

- `store_mode()` is private to store resolution and transition orchestration.
- Paused semantics are defined once for each operation family.
- Public records/chat/revision behavior and import compatibility are unchanged.
- Unit, sanitizer, TSan, database and store UI suites pass.

## P1 — `main.c` coordination bottleneck

**Status:** Complete
**Why now:** `App` owns rendering, project/session data, persistence lifecycle,
model state and several worker mailboxes. New async features increase ownership
and shutdown ambiguity.

### Evidence

- `src/main.c` is a multi-thousand-line composition root and controller.
- `App` spans `src/main.c:102-268` and mixes long-lived objects, frame strings,
  domain data, UI state and worker results.
- Backup, service, graph and name workers are coordinated directly in `App`.

### Migration plan

- [x] Extract `BackupJob`; its worker owns copied paths and never receives
      `App *`.
- [x] Extract a joinable `ServiceSupervisor`; remove detached worker access to
       `App`.
- [x] Extract entity-backfill and name-detection jobs separately.
- [x] Introduce project/session and Explorer snapshot ownership.
- [x] Extract chat and reviewed-edit controllers.
- [x] Separate a read-only frame view model from layout construction.
- [x] Establish one `app_destroy()` path for full and partial initialization.

### Acceptance criteria

- Background workers own their inputs/results and expose explicit start, poll,
  cancel/join or destroy semantics.
- `main.c` is a composition root and event loop rather than the implementation
  of every workflow.
- Clay-referenced strings retain valid frame lifetime and Explorer hit-testing
  remains snapshot-stable.

## P1 — Hidden process-global state

**Status:** Complete
**Why now:** configuration and lifecycle dependencies are established by call
order, limiting parallel tests and future independent application instances.

### Evidence

- Store configuration and lifecycle state now belong to an explicit
  `StoreManager`; manager-keyed thread contexts own connections, import state,
  timestamp overrides and verification caches.
- Semantic and recent persistence are owned services.
- Revision and finding serialization uses per-project advisory locks.

### Migration plan

- [x] Introduce an owned `StoreManager`; keep connections thread-local within
      that manager.
- [x] Introduce `SemanticClient` and clear borrowed embedder ownership.
- [x] Introduce `RecentStore`.
- [x] Make findings/revision serialization project- or manager-scoped where
      practical.
- [x] Document input and signal state that intentionally remains process-global.

### Acceptance criteria

- Service dependencies are visible in constructors or function parameters.
- Repeated app lifecycle tests can initialize and destroy state cleanly.
- Independent projects are not serialized by unrelated global state unless the
  constraint is intentional and documented.

### Intentional process-global state

- POSIX signal delivery remains a process concern: the handler only stores a
  `sig_atomic_t` shutdown request, which the single application event loop
  consumes. It owns no resources and calls no non-signal-safe code.
- Raylib exposes keyboard, mouse and system clipboard state per process/window.
  `input.c` is therefore a main-thread adapter for the one supported window;
  its scripted queue/private clipboard are test-harness state, reset by process
  lifetime. Supporting multiple windows in one process would require an owned
  input context before that product boundary changes.

## P1 — CI omits high-value verification

**Status:** Blocked on external infrastructure and repository policy
**Why now:** the repository already owns strong race, database and UI tests, but
several are optional developer actions rather than merge gates.

### Current CI coverage

- [x] Build.
- [x] Unit/headless tests.
- [x] ASan/UBSan.
- [x] TSan.
- [ ] MariaDB/Qdrant integration (`make dbtest`).
- [x] Scripted UI canary (`make uitest`, non-blocking while runner stability is
      established).
- [ ] Store UI (`make uitest-db`).
- [ ] Scheduled backup, remote and offline acceptance suites.

### Infrastructure constraint

GitHub-hosted Apple Silicon macOS runners do not provide a supported Docker
daemon or nested virtualization. `tools/services.sh` therefore cannot run the
Compose-backed MariaDB/Qdrant suite there. Do not add a nominal DB job that can
never pass.

### Migration plan

- [x] Make `make tsan` self-contained on a clean checkout.
- [x] Run TSan in CI.
- [x] Canary `make uitest` on hosted macOS; upload `build/ui` and `build/hangs`
      on failure, then make it required after proving stable.
- [ ] Provision a trusted, serialized bare-metal macOS runner with Docker for
      `dbtest` and `uitest-db`, or build a pinned native-service harness.
- [ ] Run real-model, backup, remote and offline suites on a scheduled/manual
      trusted runner.

Required-check enforcement and direct-push prevention also require repository
branch-protection or ruleset administration. The workflow can publish checks,
but this repository policy cannot be established from a source commit alone.

### Acceptance criteria

- Races and storage integration regressions cannot merge undetected.
- UI checks become required only after runner stability is demonstrated.
- Persistent self-hosted runners never execute untrusted fork code.

## P2 backlog

| Debt | Evidence | Next action |
|---|---|---|
| Observer monolith | `src/llm/observer.c` is 2,150 lines across text, model, state, jobs and worker sections. | Split along existing section boundaries after storage routing. |
| Test monoliths | `test_llm.c` and `test_db.c` span many unrelated subsystems. | Extract shared fixtures, then split by subsystem without weakening targets. |
| Documentation drift | API comments and hard-coded check counts have diverged from behavior. | Keep contracts current; avoid volatile numeric claims. |
| Weakly typed DB rows | `DbRows` exposes nullable strings by column index. | Add checked conversion helpers and typed repositories. |
| Manual dependency maintenance | Pins are strong but inventory and update reporting are manual. | Add a scheduled submodule/image/advisory report. |

## Intentional pre-alpha tradeoffs

Do not treat these as remediation targets without a changed product decision:

- macOS/Apple Silicon-only MVP and CI;
- one Makefile;
- pinned vendored dependencies;
- file and MariaDB implementations for migration/recovery/store-off mode;
- embedded SQL migrations while migration volume remains small.

## Session log

### 2026-11-05 — audit baseline and first P1 slice

- Recorded the audit as a durable register.
- Chose incremental routing rather than a complete storage rewrite.
- Added the first short-lived `ProjectStore` seam and migrated records.
- Corrected the store contract: imported projects pause structured features
  when MariaDB is unavailable; they do not merge new file writes later.
- Made TSan clean-checkout-safe and added it to CI.
- Extracted asynchronous backup ownership from `App` into `BackupJob`.
- Recorded Docker-hosting constraints before adding database CI.
- Verification: build, unit/headless, ASan/UBSan, TSan, database, base UI and
  store UI suites passed locally.

### 2026-11-05 — transition-safe routing and service ownership

- Routed revision and chat operations through the one-shot `ProjectStore` seam.
- Made import/export use explicit file backends under one transition lock and
  serialized copy-fork authority changes.
- Replaced the detached service worker with a joinable, cancellable
  `ServiceSupervisor` that owns copied inputs and structured results.
- Routed observer state/rules and entity access through `ProjectStore`, added
  capability/blocking inspection, and removed ordinary UI/domain mode checks.
- Added guarded root-document mutations, observer-state optimistic versions,
  strict import validation, and complete observer backup/restore coverage.
- Extracted entity backfill and name detection into owned, joinable jobs and
  centralized normal/partial shutdown in an idempotent `app_destroy()` path.
- Added owned project/session state and immutable, generation-tagged Explorer
  snapshots so refreshes cannot retarget document or chat hit tests.
- Extracted owned chat/attachment and reviewed-edit transition controllers,
  including explicit adoption, pending-response transfer and idempotent teardown.
- Added a generation-tagged frame string arena; every Clay text command now
  references immutable frame-owned storage through render.
- Added owned `SemanticClient` and `RecentStore` services, including isolated
  concurrent recent repositories and explicit borrowed embedder lifetime.
- Replaced process-global finding/revision mutexes with per-project advisory
  locks and final-lock finding deduplication across processes.
- Hardened the local HTTP stub used by hosted CI to consume complete requests
  and emit complete responses instead of depending on single `recv`/`send` calls.
- Verification: build, unit/headless, ASan/UBSan, TSan, database, base UI and
  store UI suites passed locally; controller ownership review found no blockers.

### 2026-11-05 — explicit store ownership and hosted UI canary

- Added an owned `StoreManager` and propagated it through project routing,
  persistence APIs, semantic/observer services, import/export/backup, workers,
  Explorer snapshots, application composition and tests.
- Keyed thread-local connections and transient store state by manager, with
  lifecycle generations that invalidate stale connections and verification
  results. Lifecycle/open transitions are serialized, failed retargets retain
  the requested probe destination, and forced-offline state is manager-local.
- Added same-thread and cross-thread dual-manager coverage, forced-offline and
  database isolation, repeated-start invalidation and verification-cache
  isolation.
- Added a non-blocking hosted `make uitest` canary with UI and hang artifacts on
  failure. Database/store UI and scheduled acceptance gates remain blocked on a
  trusted macOS service runner or pinned native-service harness; required-check
  enforcement remains blocked on repository administration.
- Verification: build and headless tests, ASan/UBSan, TSan (574 checks),
  database integration (397 checks), base UI and store UI passed locally.
