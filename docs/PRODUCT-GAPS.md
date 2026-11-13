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
| GAP-004 | Deterministic crash/recovery interruption coverage | **P0** (2026-11-11) | Done 2026-11-12 for the six transitions named: `GAP-004-interruption-matrix.md`, 129 rows (125 process deaths), 0 failures; three defects found and repaired, one finding decided and built (stray save temps are kept and offered on load); limits stated (process death is not power loss) |
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
   successor item on faithful use is a relation check on the reply. *Done
   2026-11-12* on three novels (`journal/answer-cycle4_relation.md`), with
   the verifier's tolerance and the check's two classes measured apart
   (`answer-cycle4_tolerance.md`, `answer-cycle4_relation_split.md`). **Open
   for the author:** a note turn is not gated; measured
   (`answer-cycle4_note_turns.md`), the existing checks would not catch the
   recorded embellishment and would withhold new writing, so the fix is an
   author-declared turn kind in the composer (a note the author marks as a
   note), a product affordance to decide, not a rule on the author's wording.
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
   and `make crashtest`; all six rows and the app's small-file writer hold (129 matrix rows, 125 deaths); the import and fork rows
   each found and repaired a move that a plain open never finished, the disk
   row a misleading refusal. *Done 2026-11-12* within the limits the matrix states.
   The matrix is `GAP-004-interruption-matrix.md`.
5. **GAP-003 budgets and soak.** Workload sizes and budgets agreed, then the
   soak, restart and repeatability runs on the M4; the single MEMORY-019
   baseline becomes a series. *Started 2026-11-13* (MEMORY-019, the series
   section): the baseline run five times with the fake model and three with
   the real one, every phase but the send repeatable to a few percent, the
   send's wall shown to be the reply's length (time to first token and
   tokens per second repeatable); budgets proposed from the series, awaiting
   the author. Soak with memory enabled and store sampling: two hours of the
   full cycle with the fake model steady (store settled at every sample, RSS
   flat). Restart and resume: the fake-model run clean; the real-model run
   found that a killed app's model servers kept the instance lock and no
   relaunch got the model (fixed, pinned, MEMORY-019 soak section); on the
   fixed build the real-model restart passes and a real-model soak with
   memory on is steady, with the first token 3.7 to 20.5 s away at 3,138 to
   13,179 prompt tokens; the relaunch after a crash now reaps the dead
   window's servers before sizing its context (measured: a 32K slot where
   it had run 8K). Frames above 50 ms are counted since 2026-11-13 (196 in
   97,492 with the fake model, 17 in 123,455 with the real one). Observer
   backlog measured with the observer on: none of it drained in 40 minutes
   of a send every 68 s, because the next send cancels the observer's first
   model call 12 s in and the job restarts from its beginning (journal
   `gap003-cycle1_series.md`, "the observer starves under continuous
   chat"); one observer call is about 26 s uncontended, so the remedy is a
   scheduling change for the author to choose (disclosure, cadence-aware
   starts, or a bounded hold on the send). Still open: that remedy, a time-to-first-token row
   for memory on, the Docker VM's memory cap (measured: the fit's free figure
   falls 3 GB after store work because the VM keeps the containers' cache), and the
   author's agreement on the budgets (the worst-frame row as an absolute or
   a rate).
6. **GAP-001 beyond selection.** Observer precision and recall by finding type
   on both novels; author-rated usefulness on complete sessions. *Started
   2026-11-13* (`journal/observer-cycle2_quality.md`): the observer replay
   takes the real model (`--real`); a one-chapter smoke gave 30 findings (17
   entity, 13 event, none of relationship or question) in 6 jobs at about
   22 s each; the harness (planted faults for recall by type, an audited
   fixed-seed sample for precision, thresholds agreed before tuning) is
   written before any scored run.
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

**Measured 2026-11-13, open:** the observer yields to chat by cancelling its
job and retrying it from the beginning once the chat is idle; with a send
every 68 s and 12 s pauses on the real model it completed nothing in 40
minutes and the 1,116-turn backlog stayed whole. Uncontended, one observer
call is 16.5 to 29.7 s (median 26.2) and the documents are read before the
chat. The gate is exclusive by contract, so the fix is one of three
scheduling changes for the author to choose (a disclosed interruption count,
cadence-aware starts, a bounded hold on the send), measured before adoption.

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

**Done (2026-11-12):** the matrix exists ([GAP-004-interruption-matrix](GAP-004-interruption-matrix.md))
with its method and limits; the migration rows (a process killed after every DDL statement and every
receipt recovers to the clean schema) the save row (the document is whole
at either death) the store-write row (a message is absent or present once
at either side of the commit) the import and fork rows (authority and data held
at every death; a move or fork that died after its marker is now finished at
the next open) and the disk row (full and read-only volumes refused with the
cause, data intact) are held by `make crashtest`; the limits and the one
finding that awaited a decision (stray save temps, decided 2026-11-12: kept
and offered on load as `<name> (Scribodo recovered <stamp>).<ext>`) are
recorded there.

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
