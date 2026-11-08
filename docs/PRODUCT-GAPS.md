# Product readiness gaps

**Recorded:** 2026-11-06

**Baseline:** `3e7add8`; the MVP workflows and their local/CI acceptance pass.
These are remaining product-readiness/evidence gaps, not claims that tested
features are absent. Keep the original numbering for future follow-up.

## Priorities and status

| ID | Gap | Priority | Status |
|---|---|---|---|
| GAP-001 | Broad AI usefulness and accuracy evaluation | Backlog | Deferred while GAP-003 is the focus |
| GAP-002 | Turnkey installation and release distribution | Backlog | Deferred while GAP-003 is the focus |
| GAP-003 | Whole-manuscript scale and sustained-use evidence | **P0** | Implementation underway; bounded-history foundation first |
| GAP-004 | Deterministic crash/recovery interruption coverage | Backlog | Deferred while GAP-003 is the focus |
| GAP-005 | Real macOS input, accessibility and author usability | Backlog | Deferred while GAP-003 is the focus |

The user selected **#3 as P0**. Memory/context design is a workstream within
GAP-003; it does not replace editor, database, indexing and long-session
performance measurements. The other four remain explicit follow-up work.

## GAP-001 — Broad AI usefulness and accuracy evaluation

**Gap:** Workflow tests and narrow real-model checks do not establish reliable
continuity detection and retrieval across representative novels. Entity aliases,
missed contradictions, false positives and multilingual text need measurement.

**Next work:** Assemble a held-out, annotated manuscript corpus and author tasks.
Measure finding precision/recall by type, retrieval relevance, source accuracy and
author-rated usefulness. Keep canon/draft distinctions and privacy constraints.

**Exit evidence:** A reproducible evaluation harness, versioned corpus/protocol,
documented error analysis, and explicit quality thresholds agreed before tuning.
Avoid treating one successful demonstration as a general accuracy claim.

## GAP-002 — Turnkey installation and release distribution

**Gap:** Source builds, model acquisition and Docker-backed services still impose
developer setup. Acceptance on the development Mac is not clean-machine proof.

**Next work:** Signed/notarized distribution, guided model/service setup, clear
health/error reporting, upgrade and migration handling, and uninstall behavior.

**Exit evidence:** A fresh supported Mac can install, create a project, run the
core workflows offline after setup, upgrade with existing data intact, and
recover from failed setup without source-code or shell expertise.

## GAP-003 — Whole-manuscript scale and sustained use (**P0**)

**Gap:** Runtime feasibility and correctness tests exist, but realistic large
projects, accumulated history and long editing sessions need workload-level
latency, memory, storage and observer-backlog budgets.

**Next work:**

1. Define repeatable representative and stress projects, including manuscript
   text, many chats, revisions, notes/canon and observer work.
2. Measure UI/input latency, project open/save/history/rebuild times, prompt
   preparation, inference time-to-first-token, memory, disk growth and backlog.
3. Establish a baseline on the target Apple M4/16 GB configuration, including
   resource pressure, cancellation and foreground/background contention.
4. Evaluate persistent conversation memory with bounded prompt construction;
   compare it against today's truncation/context behavior before adoption.
5. Run sustained-use/soak scenarios, restart/resume and repeatability checks.

**Exit evidence:** Explicit workload sizes and agreed performance/recall budgets;
repeatable measurements; bounded active-context and background-work costs; and
regressions covering any implemented improvements. Persistent storage is not a
claim of infinite attention or perfect recall.

**Design investigation:** `billion-context` adoption versus a native equivalent.
Keep the existing local-first, BF16 runtime, provenance and author-authority
contracts. Findings must not enter creative chat unless explicitly discussed.
The source-pinned assessment and proposed benchmark/design gates are in
[`MEMORY-001-long-lived-context.md`](MEMORY-001-long-lived-context.md).
The subsequent LMM pass proposes the MariaDB/Qdrant MVP, consistency protocols,
bounded resources and staged acceptance in
[`MEMORY-002-mvp-design.md`](MEMORY-002-mvp-design.md); it is not a runtime change.
The authorized implementation is tracked separately in
[`MEMORY-003-implementation-progress.md`](MEMORY-003-implementation-progress.md).
**Memory is P0 as of 2026-11-07.** The goal, lineage, decomposition and ordered
slices are the contract in
[`MEMORY-015-effective-memory-contract.md`](MEMORY-015-effective-memory-contract.md):
a deterministic recall view first, then a realistic fixture and two-metric
harness, then host-side selection work, then the MEMORY-003 leftovers.
DB history browsing, source identities, bounded layouts and their remaining
latency/scope limits are tracked in
[`MEMORY-004-history-windows.md`](MEMORY-004-history-windows.md).

## GAP-004 — Deterministic crash/recovery interruption coverage

**Gap:** Strong restart/fault-injection tests are not interruption coverage at
every filesystem/journal/database authority transition, nor actual power-loss
testing. See `TECHNICAL-DEBT.md` for the existing residual-risk contract.

**Next work:** Add deterministic kill points around journal writes, commits,
marker replacement and archival cleanup; include disk-full/read-only cases and
restart verification of authority, non-duplication and recoverability.

**Exit evidence:** A documented transition/failure matrix with reproducible
process-death tests and explicit limits separating them from hardware durability.

## GAP-005 — Real macOS input, accessibility and author usability

**Gap:** Scripted UI tests do not fully exercise IME composition, dead keys,
keyboard layouts, system clipboard, Finder drops or display transitions.
Accessibility and independent author usability need explicit evaluation.

**Next work:** A real-input/VoiceOver/keyboard-only test matrix and a small author
pilot on supported hardware; capture task completion, friction and failures.

**Exit evidence:** Recorded results, actionable defects with regression/manual
coverage, and author feedback on complete writing sessions rather than demos.

## Related engineering follow-ups

These do not replace or renumber the five gaps: align the Keychain requirement
with current private-file credential storage, and define release gates that
include real-model/offline acceptance on the exact release commit. Neither is
changed by this planning record.
