# Technical debt register

This is the durable cross-session register for architectural and maintenance
work. It records debt rather than individual defects. Update the status,
evidence and session log when a remediation lands.

**Last reviewed:** 2026-11-07
**Baseline:** `4060369` (`Update sample chapter opening`)
**Status:** pre-alpha; macOS on Apple Silicon is the MVP target

## Current product priority

The five post-audit product-readiness gaps are tracked separately in
[`PRODUCT-GAPS.md`](PRODUCT-GAPS.md). **GAP-003, whole-manuscript scale and
sustained-use evidence, is P0 by user direction.** The other four are retained
as deferred follow-ups; completed architectural debt below is historical.

## Status legend

- **Open** — accepted debt, no implementation started.
- **In progress** — a migration seam exists and further call sites remain.
- **Blocked** — the next step needs infrastructure or a decision.
- **Done** — acceptance criteria are met; retain the entry as history.

## Accepted residual risks — persistence review and direct-push workflow

**Recorded:** 2026-11-06
**Status:** Open; follow-up considerations, not known release blockers.
**Review reference:** `b40dcb0`; see `REDTEAM-011-legacy-fork.md` and
`REDTEAM-012-marker-snapshots.md` for evidence and scope.

### CI reports after direct pushes

The repository now permits direct pushes to `main`. CI still runs on pushes, but
PR and required-status-check gating were removed. A failing change can therefore
reach `main` before CI reports it. Force-push, branch-deletion and linear-history
protections remain enabled.

- **Mitigation:** validate locally before pushing, check the resulting CI run,
  and promptly fix failures with a follow-up commit.
- **Future consideration:** restore pre-merge gating if contributors or release
  requirements grow; reassess this workflow before relying on `main` for releases.

### Power-loss recovery testing is not exhaustive

Regression, restart, fault-injection, sanitizer and mutation tests cover the
documented recovery paths. They do not simulate process death or power loss at
every filesystem operation, nor every filesystem/hardware durability behavior.

- **Future work:** add deterministic interruption points around journal writes,
  transaction commits, marker replacement and archival cleanup. Verify restart
  behavior and authority preservation at each boundary, distinguishing process
  termination tests from actual power-loss/durability testing.

### Snapshot fingerprints are not a tampering boundary

Fork snapshot fingerprints use filesystem metadata to detect ordinary changes
during retry/recovery. They do not authenticate content against deliberate local
tampering; metadata can be preserved or manipulated.

- **Current assumption:** local project metadata is not an adversarial trust
  boundary. Existing fingerprint checks must not be described as authentication.
- **Future consideration:** if the threat model expands, evaluate content
  digests, trusted manifests and safe path handling together. A digest stored
  beside writable content alone does not protect against an attacker who can
  rewrite both.

No unresolved finding remained within the completed review's scope. This does
not establish universal correctness or remove the limitations above.

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

- cJSON's parser owns process-global error state even when callers ignore it.
  All application and test parsing uses `src/core/json.c`, which serializes the
  parse call only; returned trees remain caller-owned. Allocator hooks and locale
  must not change while workers run, and callers must not use `cJSON_GetErrorPtr`.
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
`.github/workflows/acceptance.yml` accepts only repository-owner dispatch, refuses
fork pull-request code, serializes jobs and uploads UI/hang diagnostics on failure.
The repository currently has no other collaborators; adding one requires review
of this trust policy before that account receives write access.

Acceptance uses a disposable `scribodo-ci` Compose project, job-temporary secrets,
alternate loopback ports and always-run volume cleanup, so it neither reads normal
application service credentials nor shares the development stack. Repository
Actions policy permits only GitHub-owned actions and requires immutable SHA pins.
Runner metadata, credentials and work directories are owner-only on disk.

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

Repository administration enforces linear history and blocks force-pushes and
branch deletion on `main`. Pull requests and required status checks are **not**
enforced: direct pushes are permitted, and CI reports after the fact. See
"CI reports after direct pushes" above for the accepted risk and mitigation.

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

### 2026-11-06 — trusted-runner red-team remediation

- Found that the first acceptance workflow shared the normal Compose identity,
  ports, volumes and long-lived Scribodo service credentials. Acceptance now uses
  a disposable project on ports 33062/6334 with job-temporary secrets and purges
  containers and volumes even after failure.
- Propagated Compose project and Qdrant port configuration through service,
  backup, restore and disaster-recovery tools; database administration tests now
  honor both configured ports and secret roots. The isolated DB, store UI, backup
  and remote-provider suites pass together and leave no containers or volumes.
- Restricted persistent-runner dispatch to the repository owner as well as
  excluding forks. Verified that the owner is the repository's only collaborator.
- Enforced repository-level SHA pinning and GitHub-owned Actions only, and changed
  runner credentials, metadata and work directories to owner-only permissions.

### 2026-11-06 — persistence audit remediation

- Reproduced cross-chat data mixing in concurrent file readers. Replaced every
  application `strtok()` call with caller-owned tokenizer state. A new concurrent
  chat/revision regression initially failed against the original implementation.
- The expanded TSan test then exposed cJSON's shared parser error state. Routed
  parsing through one serialized application boundary without modifying the
  pinned dependency; concurrent valid and crash-torn JSON is now covered.
- Made failed chat enumeration/loading abort and roll back migration, preserving
  file authority and allowing repair/retry. Chat reads reject short reads and
  non-regular files; attachment/id-map allocation failures also abort import.
- Replaced the export's error-collapsing record list with the checked API. Database
  regressions cover decode failures for notes, canon and findings, as well as
  successful export after repair.
- Aligned backup creation with restore's regular-file-only policy: reject links
  and special files, refuse recursive destinations, reserve a fresh destination,
  verify before reporting success and clean up failed output. Checksum generation
  now includes nested author files named `SHA256SUMS`. Headless backup regressions
  run under the normal watchdog-wrapped test target.
- Verification on the final code: build/headless, ASan/UBSan (including database
  integration), TSan, database integration, base UI, store UI, backup/disaster
  recovery, stand-in remote-provider acceptance and dependency reporting passed.
  Real-model/offline acceptance was not rerun for this persistence-focused change.

### 2026-11-06 — persistence remediation red-team

- Adversarial tests found that verification ignored inaccessible extra
  directories. Both filesystem traversal passes now propagate errors.
- Chat loading now validates the actual opened regular-file descriptor, rejects
  raw/decoded NUL truncation and malformed recognized messages, and distinguishes
  omitted legacy timestamps from wrong-type timestamps. Nonempty attachment
  allocation failure no longer falls back to an empty attachment list.
- File chat persistence now preserves truncation flags and fractional message
  times. Rename timestamps use matching precision so a rename cannot round down
  before an earlier chat source in the same millisecond.
- Explicit exports now require fresh structured metadata and hold an exclusive
  transition lock. A failed or completed export cannot be appended to on retry;
  callers use a new destination, and in-app backups already own and clean theirs.
- Ten scratch-copy mutations were compiled/executed or exercised through the
  backup suite; all ten produced assertion failures (not crashes or timeouts).
  Detailed scope, regression evidence and validation are recorded in
  `docs/REDTEAM-010-persistence.md`.
- Final-code verification passed: build/headless, ASan/UBSan, sanitized database
  integration, TSan, database integration, base/store UI, backup/disaster recovery,
  stand-in remote-provider acceptance and dependency reporting. A second scoped
  review and the final mutation rerun found no remaining confirmed defects in the
  reviewed changes. Real-model/offline suites remain unavailable without the
  built local runtime; no claim of their completion is made.

### 2026-11-06 — legacy import, allocation and fork recovery closure

- Reproduced the remaining file-backend empty/error conflation. Added complete
  regular-file reads and checked record/revision listings, migrated all production
  consumers, and made reads/writes/import fail without partial results. Imports
  now reject unreadable or damaged snapshots instead of silently skipping them.
- Added an isolated allocation-failure binary covering parser and repository
  allocations plus JSON serialization. A local cJSON compilation wrapper records
  OOM per thread without changing pinned source or runtime allocator hooks.
- Replaced in-place fork exports with journaled, consistent source snapshots.
  Retries preserve one completed snapshot across failures/restarts, never append
  partial output, and preserve unrecognized staging. Successful snapshots are
  archived; copied post-commit journals can finish cleanup.
- Found and reproduced copied pending-state ownership and cross-document snapshot
  filename identity bugs. Pending IDs now require canonical path ownership, and
  revision-source mapping includes the document. Registration/journal updates are
  atomic and synced; authoritative query failures abort rather than select a
  fallback destination.
- Seven compiled scratch mutations were caught by assertions. Evidence and limits
  are in `docs/REDTEAM-011-legacy-fork.md`.
- Built the pinned local runtime using cached CMake, without downloading weights
  or web assets. The previously outstanding real-model and network-isolated
  acceptance suites passed on the first full run and again on final production
  code. Final build/headless, database, ASan/UBSan (including database), TSan,
  base/store UI, backup/restore, stand-in remote-provider, offline real-model and
  embedding acceptance, dependency reporting and diff checks all passed. The
  expanded one-shot/sustained allocation suite passed 680 checks normally and
  under ASan/UBSan; all seven final mutations were caught. The three outstanding
  concerns are closed within this review's documented scope.

### 2026-11-06 — follow-up adversarial marker/snapshot review

- Found a fork crash window caused by temporarily withdrawing the copy marker.
  Removed marker withdrawal/restoration: retain copy authority until the atomic
  target-marker replacement. The baseline fails the new occupied-set-aside-path
  regression; the remediation preserves that unrelated path and succeeds.
- Reproduced immutable snapshot overwrite when all 1,000 candidate filenames
  were occupied. Exhaustion now fails before writing; `lstat` treats dangling
  links as occupied and inspection errors fail closed.
- Added paused-write and non-monotonic-clock export/import checks. Nine compiled
  mutations were caught by assertions, including restoration of both defects.
- Headless, database, ASan/UBSan (headless and database), TSan, and dependency
  validation passed. Full native/offline acceptance is running before publication.
  Scope, evidence and limits: `docs/REDTEAM-012-marker-snapshots.md`.
- Second iteration reproduced false-success reuse of missing/damaged snapshots.
  Reuse now verifies actual content under the revision lock, including older
  source snapshots. Missing/damaged, repaired and older-candidate regressions
  pass. Ten compiled mutations were caught. Final headless (3,552 adapter checks
  plus 680 isolated allocation checks), database (590 repository checks plus 37
  core checks), ASan/UBSan and TSan reruns passed after this change.
- Final production-code rerun of `make all uitest uitest-db backuptest remotetest
  offlinetest` passed, including real BF16 chat and embedding workflows under
  network isolation. Final scoped diff review found no further unresolved issue;
  all ten mutation checks passed. Publishing uses required PR checks without
  changing branch protections. The pre-existing sample-document edit is excluded.

### 2026-11-06 — editor, embedding and startup failure-path remediation

- Reproduced three audit findings with failing regressions before remediation:
  serialization OOM consuming an uninitialized embedding result, failed gap
  growth deleting a replacement's target, and FIFO configuration blocking startup.
- Preflighted editor net growth before selection/undo/anchor changes. Embedding
  request construction now fails closed, and complete batch validation precedes
  output publication. Remote configuration/key reads validate nonblocking opened
  regular-file descriptors, permissions, complete reads and size/NUL boundaries.
- Added isolated one-shot/sustained editor and embedding allocation suites to
  normal and ASan/UBSan tests; the embedding transport suite also runs under TSan.
  Nine compiled scratch mutations were caught by assertions, not crashes/timeouts.
- Final verification passed: fresh application/headless build (22,590 C checks
  plus 11 Python tests), ASan/UBSan, TSan, normal/sanitized database integration,
  base/store UI, backup/restore, stand-in remote, case-sensitive rename, offline
  real-model/embedding acceptance, dependency reporting and diff checks. Clang
  static analysis of the three changed production modules emitted no diagnostics.
- Evidence and limits: `docs/REDTEAM-013-failure-paths.md`. A successful edit can
  still clear undo history if recording that edit runs out of memory; the
  inconsistent history is deliberately discarded rather than replayed. The
  pre-existing sample-document edit remains excluded.

### 2026-11-06 — failure-path adversarial follow-up and publication

- Committed the initial validated remediation as `720f5b0`, then added
  adversarial regressions for complete JSON consumption and service-secret alias
  exclusion. The pre-fix follow-up run failed 11 remote and four embedding
  assertions; no crash or timeout was counted as a valid regression result.
- Added a serialized complete-input JSON boundary for configuration and embedding
  adapters without changing legacy crash-torn log parsing. Trailing bytes,
  extra JSON values and NUL-decoded field/value aliases are refused. Dangling
  configuration links and path/field truncation have explicit error paths.
- Checked opened key descriptors against canonical configured/default service
  roots and known credential device/inode identities, refusing hard-link aliases
  and unverifiable metadata before reading credential contents.
- A second scoped review found no further confirmed defect. All 15 compiled
  mutation checks were caught by assertions. A guarded-span ASan/UBSan probe
  exercised 20,000 random non-terminated inputs without an out-of-span read.
- Final validation passed: fresh application/headless build (22,639 C checks and
  11 Python tests), ASan/UBSan, TSan, normal/sanitized database integration,
  dependency checks, four-module static analysis, and serialized native/recovery/
  offline acceptance (42 script runs, 628 expectations, including real models).
- Evidence and residual limits remain in `docs/REDTEAM-013-failure-paths.md`.
  The unrelated sample-document edit is neither modified nor included in commits.

### 2026-11-06 — product gap priorities and long-lived memory investigation

- Recorded all five product-readiness gaps in `docs/PRODUCT-GAPS.md`; GAP-003
  (whole-manuscript scale and sustained use) is P0 by user direction. The other
  four remain deferred, individually scoped and paired with exit evidence.
- Inspected `ranxianglei/billion-context` at commit
  `c3dfbe6b21a1b2fc3642594043bc81fd2ae91247`, including its kernel design,
  licenses and reported benchmark limitations. No upstream package was installed
  or executed. The inspected licenses add visible attribution to MIT text.
- Recommended evaluating a native bounded context planner using retained raw
  sources, hierarchical briefs and exact/semantic retrieval. Request compression
  alone does not bound host history loads, copies, UI caches or background work.
- Recorded the source-pinned assessment, privacy/authority invariants, workload
  sweeps and baseline/retrieval/hybrid comparison plan in
  `docs/MEMORY-001-long-lived-context.md`. This is design work only: no inference
  behavior changed and no benchmark results or infinite-recall claims are made.
- Validated the new Markdown's local link targets and whitespace. The existing
  sample edit remains untouched; implementation and numeric performance budgets
  are next-step decisions.

### 2026-11-06 — bounded memory foundation and adversarial remediation

- Implemented bounded owned request-history snapshots and byte-limited MariaDB
  source traversal. Reviewed edits no longer copy unrelated chat history.
- Adversarial regressions exposed mandatory-request omission, non-atomic chat
  loading/preparation, lost metadata and an aggregate high-water query. Fixed
  them and added allocation sweeps, deterministic properties, query-plan and
  combined-notice checks. Five selector mutations are detected.
- Validation and limits are recorded in `REDTEAM-014-memory-foundation.md` and
  `MEMORY-003-implementation-progress.md`. Unit, real database, sanitizer and
  scripted UI suites pass; this is not a full memory MVP or recall-quality claim.
- UI history/cache windowing and stable source-navigation mappings remain next,
  before ingestion/outbox/retrieval and T1/T2 experiments. GAP-003 remains P0.
- The sample-document edit remains untouched and excluded from commits.
- Post-commit review of `3a4fa35` repeated mutation checks and added explicit
  null/value guards for clean static analysis of the adapter and new history
  modules. The final code was revalidated before the closeout commit/push; see
  `REDTEAM-014-memory-foundation.md` for the matrix and remaining scope.

### DB history windows and bounded layouts

- Added byte-limited DB display windows, old/new/latest navigation and navigable
  large-message excerpts. Source links use committed message sequences, not
  window positions. Unsaved DB turns cannot be paged away or silently evicted.
- Bounded display decoding/wrapping and reset caches/actions on replacement.
  Allocation sweeps cover failed layout rebuilds; originals are not truncated.
- Added growing real-service fixtures, a 400-message source-navigation UI test,
  cache stress/accounting and warm diagnostic timings. Details and limits are
  in `MEMORY-004-history-windows.md`.
- Navigation is still synchronous; observer/file/backup compatibility readers,
  sustained-use and percentile latency gates remain explicit follow-up work.
  No memory jobs, Qdrant projection or generated briefs are implemented yet.

### History-window adversarial follow-up

Asynchronous navigation and subsequent adversarial fixes are now recorded in
`MEMORY-005-async-history.md`: owned/coalesced reads, cancellation/publication
guards, local identity/store epochs, attachment revisions, unlocked worker
connection establishment and malformed-marker fail-closed routing. A 500-sample
DB diagnostic is reproducible with `make historysoak`; it is not hours-long
sustained-use or UI latency evidence. In-flight I/O interruption, other synchronous
UI/store paths and full-history compatibility consumers remain debt.

- Reproduced stale presentation selecting a different reply/action, archive-sized
  slot retention after trimming/clear, blank window replacement on empty
  navigation, and unsaved DB chat loss on project switch/close.
- Added presentation revision guards, capacity release/retry, empty-range refusal
  and explicit unsaved lifecycle/discard protection. Layout/retention failure
  tests and outage-driven store UI regressions cover the remediations; TSan now
  includes the chat view with a concurrent fake worker.
- Evidence and residual limits are in `REDTEAM-015-history-windows.md`. No change
  to the pinned model, remote data-exit policy, memory-MVP scope or deferred
  asynchronous/soak requirements is implied. The sample edit remains excluded.
- A second pass reproduced stopped-reply flags being lost by both persistence
  backends. Migration 008 and file/DB/import/export/source/window round trips now
  preserve them; unrecoverable historical flags default false.
