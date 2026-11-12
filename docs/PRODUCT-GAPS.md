# Product readiness gaps

**Recorded:** 2026-11-06
**Re-reviewed:** 2026-11-09 against `af57a32`; no gap status changed.
**Re-prioritised:** 2026-11-11 by the author after [LANDSCAPE](LANDSCAPE.md): every remaining gap is P0. The order below is the working order, not a ranking of importance.

**Baseline:** `3e7add8`; the MVP workflows and their local/CI acceptance pass.
These are remaining product-readiness/evidence gaps, not claims that tested
features are absent. Keep the original numbering for future follow-up.

## Priorities and status

| ID | Gap | Priority | Status |
|---|---|---|---|
| GAP-001 | Broad AI usefulness and accuracy evaluation | **P0** (2026-11-11) | Started: a second, held-out manuscript (A Princess of Mars) with questions written before any run; see the 2026-11-11 section |
| GAP-002 | Turnkey installation and release distribution | **P0** (2026-11-11) | Not started |
| GAP-003 | Whole-manuscript scale and sustained-use evidence | **P0** | Implementation underway; bounded-history foundation first; memory contract in MEMORY-015 |
| GAP-004 | Deterministic crash/recovery interruption coverage | **P0** (2026-11-11) | In progress: kill-point harness and the migration, document-save, store-write and legacy-import rows done 2026-11-12 (`GAP-004-interruption-matrix.md`, 113 deaths, 0 failures; one finding repaired, one awaiting a decision); fork cleanup and disk rows open |
| GAP-005 | Real macOS input, accessibility and author usability | **P0** (2026-11-11) | Not started |

The user selected **#3 as P0** on 2026-11-06 and made the remaining four P0 on
2026-11-11.

## Working order from 2026-11-11

Every gap is P0; this is the order the records argue for, each item with the
measurement that closes it. Items run one at a time unless marked otherwise.

1. **Faithful use: window construction** (LANDSCAPE row 13). *Done
   2026-11-11* (`journal/answer-cycle4_window.md`): earlier refusals are shown
   to the composer as a neutral marker on question turns, measured offline on
   the captured requests (72 generations, exact reproduction of recorded
   replies) and live on both novels. The walk turn answers correctly; the
   Charlotte turn still declines, for a misread relation, not the window. The
   successor item on faithful use is a relation check on the reply.
2. **Generalisation: the second manuscript** (GAP-001, LANDSCAPE rows 12 and
   20). *A Princess of Mars*, imported, with 8 boundary cases and 15 reach
   tasks written before any run. Run reach and the verified pipeline once at
   the current build and record the numbers beside the first novel's. Never
   tune against this fixture; a third novel becomes the held-out set if this
   one is ever used for tuning.
3. **Experiment flags and columns** (TECHNICAL-DEBT P1). Retire the two
   decided flags, read the rest once in `main.c`, decide migration 015's
   column. Runs alongside 1 and 2 because every further experiment adds to it.
4. **GAP-004 crash points.** Deterministic kill points around journal writes,
   commits, marker replacement, archival cleanup and migration receipts;
   disk-full and read-only cases. Closes with the transition/failure matrix. *Started
   2026-11-11* (`journal/gap004-cycle1_migration.md`): `core/crash_point.h`
   and `make crashtest`; the migration, save, store-write and import rows hold at every one of 113
   deaths; the import row found and repaired an unfinished move.
   The matrix is `GAP-004-interruption-matrix.md`.
5. **GAP-003 budgets and soak.** Workload sizes and budgets agreed, then the
   soak, restart and repeatability runs on the M4; the single MEMORY-019
   baseline becomes a series.
6. **GAP-001 beyond selection.** Observer precision and recall by finding type
   on both novels; author-rated usefulness on complete sessions.
7. **GAP-002 distribution.** Signed app, guided model and service setup,
   upgrade and uninstall, proven on a clean Mac.
8. **GAP-005 real input and accessibility.** The IME, dead-key, layout,
   clipboard and VoiceOver matrix and a small author pilot.

Items 7 and 8 come last because they package and polish what items 1 to 6
make true; shipping them earlier would ship the current composer variance
to an author. Memory/context design is a workstream within
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

**Progress (2026-11-11):** the matrix exists ([GAP-004-interruption-matrix](GAP-004-interruption-matrix.md))
with its method and limits; the migration rows (a process killed after every DDL statement and every
receipt recovers to the clean schema) the save row (the document is whole
at either death) the store-write row (a message is absent or present once
at either side of the commit) and the import row (authority and data held at
every death; a move that died after its marker is now finished at the next
open) are held by `make crashtest`; the other rows are open and
listed there, with one finding awaiting a decision (stray save temps).

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
