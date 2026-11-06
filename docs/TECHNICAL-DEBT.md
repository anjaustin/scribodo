# Technical debt register

This is the durable cross-session register for architectural and maintenance
work. It records debt rather than individual defects. Update the status,
evidence and session log when a remediation lands.

**Last reviewed:** 2026-11-06
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

- `src/main.c` is a large composition root and controller.
- `App` mixes long-lived objects, frame strings,
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

**Status:** Done for build, sanitizer, race, database and UI merge coverage;
real-model/offline acceptance remains an explicit manual operation
**Why now:** the repository already owns strong race, database and UI tests, but
several are optional developer actions rather than merge gates.

### Current CI coverage

- [x] Build.
- [x] Unit/headless tests.
- [x] ASan/UBSan.
- [x] TSan.
- [x] MariaDB/Qdrant integration (`make dbtest`) on the trusted Mac runner.
- [x] Scripted UI (`make uitest`) on the trusted Mac runner; the hosted canary proved the runner has
      no display session in which Raylib can create a window, so the permanently
      failing job was retired rather than consuming capacity without coverage.
- [x] Store UI (`make uitest-db`) on the trusted Mac runner.
- [x] Scheduled/manual backup and remote-provider acceptance suites.
- [ ] Real-model/offline acceptance remains manual because it requires the local
      model asset and a deliberately network-isolated sandbox.

### Infrastructure constraint

GitHub-hosted Apple Silicon macOS runners do not provide a supported Docker
daemon or nested virtualization. `tools/services.sh` therefore cannot run the
Compose-backed MariaDB/Qdrant suite there. Do not add a nominal DB job that can
never pass. The first hosted UI canary (run `42157320138`) also confirmed that
these runners have no usable display session: every script stopped at window
creation. Failure artifacts were uploaded successfully, but the UI job cannot
become required on this runner class.

A repository-scoped ARM64 runner named `scribodo-mac-studio` now runs as a user
LaunchAgent on the trusted development Mac. Docker Desktop supplies the pinned
MariaDB/Qdrant services and the logged-in WindowServer supplies UI execution.
`.github/workflows/acceptance.yml` refuses fork pull-request code; trusted jobs
are serialized and upload UI/hang diagnostics on failure.

### Migration plan

- [x] Make `make tsan` self-contained on a clean checkout.
- [x] Run TSan in CI.
- [x] Trial `make uitest` on hosted macOS with `build/ui` and `build/hangs`
      failure artifacts; retire the trial after it proved the runner class has
      no usable display.
- [x] Provision a trusted, serialized bare-metal macOS runner with Docker for
      `dbtest`, `uitest` and `uitest-db`.
- [x] Run backup and remote-provider suites on scheduled/manual trusted runs.
- [ ] Keep real-model/offline verification manual until model asset provisioning
      and sandbox policy are made reproducible on the runner.

Repository administration now enforces pull requests, linear history and the
hosted build/sanitizer/TSan checks plus trusted acceptance on `main`.

### Acceptance criteria

- Races and storage integration regressions cannot merge undetected.
- UI checks become required only after runner stability is demonstrated.
- Persistent self-hosted runners never execute untrusted fork code.

## P2 backlog

| Debt | Status | Evidence | Next action |
|---|---|---|---|
| Observer monolith | Done | Text analysis, deterministic/model backends and job discovery/state pruning are separate modules; `observer.c` retains candidate application plus worker synchronization, cancellation and generations. | Keep worker ownership cohesive unless a narrower interface emerges. |
| Test monoliths | Done | Pure observer/DB suites, stateful components and remote/network behavior run independently; shared fixtures own temporary roots and stores, and database core/migrations are a separately registered suite. | Add future subsystem tests to the narrowest existing binary. |
| Documentation drift | Done | Living guides no longer encode volatile source locations or assertion totals; dated red-team reports retain their point-in-time evidence. | Keep contracts current and reserve numeric evidence for dated reports. |
| Weakly typed DB rows | Done | Production code has no direct indexed-cell reads; repository decoders validate shape, nullability, type and domain. Checked record/revision list failures propagate through `ProjectStore`; compatibility count wrappers remain source-compatible but have no production callers. | Require local decoders and checked routing for every new query. |
| Manual dependency maintenance | Done | `make dependency-report` and a weekly workflow inventory submodules and images, verify immutable pins, and query OSV by vendored commit. | Review informational upstream updates and remediate any report failure. |

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
  failure. Its first remote run confirmed that hosted macOS has no display
  session (`could not create a window` for all scripts), while artifact upload
  and the gated build/headless/sanitizer jobs worked. Database, base/store UI
  and scheduled acceptance gates remain blocked on a trusted macOS runner or
  pinned native-service harness; required-check enforcement remains blocked on
  repository administration.
- Verification: build and headless tests, ASan/UBSan, TSan, database integration,
  base UI and store UI passed locally.

### 2026-11-05 — StoreManager red-team follow-up

- Made every owned service and worker that borrows `StoreManager` reject a null
  dependency instead of silently degrading to file-only/default behavior.
- Retired the hosted UI canary after two artifact-producing runs consistently
  failed before script execution because the runner had no display. The durable
  register retains that verified infrastructure constraint without creating a
  permanently failing, non-gating job.
- Verification: build and headless suites, ASan/UBSan, TSan, database
  integration, base UI and store UI passed locally.

### 2026-11-05 — P2 maintenance truthfulness

- Removed volatile source locations and assertion totals from living guides while
  preserving dated red-team evidence as historical records.
- Added an owned dependency report for submodule pins, upstream availability,
  container tag/digest integrity and OSV commit advisories, plus a weekly/manual
  workflow that publishes the Markdown result.
- Updated the pinned checkout and artifact actions to their Node 24 releases,
  and advanced cJSON to its current post-1.7.19 revision after the first report
  identified six advisories on the previous pin. Scribodo does not compile
  `cJSON_Utils` or call `cJSON_Compare`, and its supported 64-bit target is not
  exposed to the reported 32-bit print overflow; the newer pin also includes
  upstream depth and utility hardening.
- Added checked `DbRows` shape, nullability and numeric conversions with focused
  overflow/malformed-value tests, then moved the chat repository to local typed
  row decoders instead of unchecked column coercion.
- Split pure evidence/overlap analysis and the deterministic observer backend
  from worker orchestration. Their tests now run as independent binaries, which
  begins the test-monolith migration without exposing `Observer` worker state.

### 2026-11-06 — checked revision and record repositories

- Replaced unchecked revision, record-version and source-reference row access
  with repository-local decoders for exact shapes, required/nullable text,
  bounded domain fields, finite timestamps and overflow-safe integer values.
- Malformed rows and allocation failures now discard the whole decoded result
  rather than returning partially populated record history.
- Made schema-version reads use the shared checked integer conversion.
- Added socket I/O deadlines to the remote test stub and corrected a reset
  callback counter that otherwise forced the test's full watchdog fallback.
- Verification: build/headless, MariaDB/Qdrant integration, ASan/UBSan, TSan,
  dependency reporting, base UI and store UI suites passed locally.

### 2026-11-06 — P2 decoder red-team remediation

- Found that syntactically checked chat values could still overflow integral
  domain conversions, silently truncate fixed-size summary fields, or leave
  related document rows incompletely consumed. Added exact exclusive numeric
  bounds, field-fit checks, ordered non-negative sequences and complete relation
  consumption, with database corruption tests that assert no partial output.
- Hardened `DbRows` shape, cleanup and query-allocation arithmetic against
  multiplication and buffer-size overflow. Boundary tests cover malformed
  synthetic shapes and the exactly out-of-range `2^63` timestamp.
- Made attachment-copy allocation failure roll back the chat transaction instead
  of silently committing a message without its requested document references.
- Found that dependency reporting treated a submodule's ambient checkout as the
  repository pin. It now enforces committed gitlink equality and a clean tracked
  checkout, while upstream availability remains informational. A deliberately
  mismatched cJSON checkout was detected and the original pin restored.
- Increased accepted test-socket I/O deadlines from two to ten seconds: teardown
  remains bounded without imposing an unnecessarily narrow sanitizer deadline.
- Remaining limitation: legacy count-returning revision and record list APIs
  still collapse decode failure and an empty result; this remains explicit P2
  work rather than being claimed complete.

### 2026-11-06 — P2 completion

- Migrated every remaining production query in semantic indexing, observer
  state, entities, import/export and store identity to checked local decoding;
  a source search now finds no direct `db_cell` reads under `src/`.
- Added checked record and revision list APIs through database backends and the
  `ProjectStore` routing seam, retaining count-returning compatibility wrappers.
- Split observer model execution and job discovery/state pruning from the worker
  owner. Mutexes, cancellation, generations and candidate application remain
  together rather than leaking through a broad internal header.
- Added a shared stateful test fixture; split remote/network and component suites
  into independent binaries; and separated database core/migration registration
  while preserving translation-unit-local failure accounting.
- Expanded TSan to every stateful LLM/component/remote binary so decomposition
  does not reduce race coverage.

### 2026-11-06 — P2 completion red-team

- Found that extracted observer job discovery conflated clean exhaustion with
  filesystem, allocation and chat-list failures. Those failures could persist
  pruned state or advance past an unread message. Discovery now reports errors
  separately, owns chat copies atomically, retains state and retries with backoff.
- Made file-chat loading all-or-nothing across message, attachment and provider
  allocations, with strict roles/timestamps; short document reads now fail and
  retry instead of treating a truncated snapshot as complete.
- Hardened observer JSON progress updates so allocation failures cannot mark a
  document or chat read, and bounded persisted numeric positions before casts.
- Found that semantic rebuild treated document, chat and record-count queries as
  optional. Every authoritative query is now required; failure leaves rebuild
  pending for a later retry.
- Added checked-list corruption regressions for over-width revision origins and
  record statuses, plus an observer discovery-error regression.

### 2026-11-06 — trusted acceptance and enforced checks

- Registered a repository-scoped ARM64 macOS runner backed by the development
  Mac's Docker Desktop and graphical login session.
- Added serialized trusted acceptance for database integration and base/store UI
  suites on pushes and repository-owned pull requests. Fork pull-request code is
  never dispatched to the persistent runner; scheduled/manual runs also exercise
  backup and stand-in remote-provider recovery.
- Migrated every production record/revision list call to checked APIs. UI loads
  now report failure rather than presenting false empty state, and observer work
  aborts/retries rather than acting on incomplete records.
- Enabled `main` branch protection with required pull requests, linear history,
  administrator enforcement, and hosted plus trusted acceptance checks.
