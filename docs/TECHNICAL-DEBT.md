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

**Status:** In progress
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
- [ ] Extract entity-backfill and name-detection jobs separately.
- [ ] Introduce project/session and Explorer snapshot ownership.
- [ ] Extract chat and reviewed-edit controllers.
- [ ] Separate a read-only frame view model from layout construction.
- [ ] Establish one `app_destroy()` path for full and partial initialization.

### Acceptance criteria

- Background workers own their inputs/results and expose explicit start, poll,
  cancel/join or destroy semantics.
- `main.c` is a composition root and event loop rather than the implementation
  of every workflow.
- Clay-referenced strings retain valid frame lifetime and Explorer hit-testing
  remains snapshot-stable.

## P1 — Hidden process-global state

**Status:** Open
**Why now:** configuration and lifecycle dependencies are established by call
order, limiting parallel tests and future independent application instances.

### Evidence

- `store.c` owns global configuration/online state and thread-local connection,
  import, timestamp and verification state.
- `semantic.c`, `recent.c` and `input.c` hold process-global configuration.
- Revisions and findings use process-global mutexes.

### Migration plan

- [ ] Introduce an owned `StoreManager`; keep connections thread-local within
      that manager.
- [ ] Introduce `SemanticClient` and clear borrowed embedder ownership.
- [ ] Introduce `RecentStore`.
- [ ] Make findings/revision serialization project- or manager-scoped where
      practical.
- [ ] Document input and signal state that intentionally remains process-global.

### Acceptance criteria

- Service dependencies are visible in constructors or function parameters.
- Repeated app lifecycle tests can initialize and destroy state cleanly.
- Independent projects are not serialized by unrelated global state unless the
  constraint is intentional and documented.

## P1 — CI omits high-value verification

**Status:** In progress
**Why now:** the repository already owns strong race, database and UI tests, but
several are optional developer actions rather than merge gates.

### Current CI coverage

- [x] Build.
- [x] Unit/headless tests.
- [x] ASan/UBSan.
- [x] TSan.
- [ ] MariaDB/Qdrant integration (`make dbtest`).
- [ ] Scripted UI (`make uitest`).
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
- [ ] Canary `make uitest` on hosted macOS; upload `build/ui` and `build/hangs`
      on failure, then make it required after proving stable.
- [ ] Provision a trusted, serialized bare-metal macOS runner with Docker for
      `dbtest` and `uitest-db`, or build a pinned native-service harness.
- [ ] Run real-model, backup, remote and offline suites on a scheduled/manual
      trusted runner.

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
