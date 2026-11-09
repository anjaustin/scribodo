# MEMORY-022 — Product remediation, measured locally

**Date:** 2026-11-09. **Baseline:** `278259c`.

**Scope:** functional product work, not CI. CI configuration is unchanged.
This is not a completion score or a claim that PRODUCT-GAPS is closed.

## Fixed measurement contracts

- Keep the novel, questions, answer keys and seed fixed. Report exact key-text
  reach separately from the existing, looser answer-alias measure.
- Compare complete real-app replies separately from source reach. Inspect all
  clauses of an answer; a matching word is not a semantic quality pass.
- Verify every admitted byte range against its original, including document
  tasks. Previously document tasks unconditionally reported zero violations.
- Check the actual rendered 16,384-byte and 4,096-token limits.
- Preserve failed measurements. Do not change source eligibility, author
  authority, remote consent, pinned models or default-off memory to get a pass.

`tools/memory_reach_compare.py BEFORE AFTER --output FILE` rescores both frozen
runs with the same scorer, verifies equal input/answer-key hashes, and reports
integer task counts, source-range violations, limits and measured timings.
Fresh test databases have different source IDs, so tied discovery scores can
change order. Timings describe these runs, not an isolated benchmark or SLA.

## Fresh baseline and rejected windowing attempts

The 30K chat fixture, seed 20261107, three tasks/category, and the unchanged
14 novel questions in `pride-and-prejudice-tasks.json` were run with the real
tokenizer and embedder. There are 41 questions: 24 measured chat tasks,
14 document tasks, and three absence tasks with no source-reach score.

| Arm | Exact key text admitted | Answer alias admitted | Chat tasks with every required source | Source-range violations |
|---|---:|---:|---:|---:|
| Original | 10/14 | 10/14 | 24/24 | 0 |
| Single query window | 10/14 | 12/14 | 24/24 | 0 |
| All anchor windows with neighbouring context | 10/14 | 10/14 | 24/24 | 0 |

Both windowing attempts fail the registered 14/14 exact-key target. The single
window recovered some passages but removed others' answer-bearing context;
its higher alias count is not counted as success. The multi-window arm did
not improve reach. It is retained for reproducibility behind
`SCRIBODO_MEMORY_EXCERPTS=1`, **off by default**, not silently enabled for authors.

The stricter scorer also measured **13/14 exact key passages discovered** in
all three arms. The former 14/14 candidate measure allowed alternate answer
aliases; it did not establish discovery of all designated key passages.

Evidence under `build/memory-reach/runs/`:

- `scribodo_test_reach_74605_b87de8bb89e09cce` — original.
- `scribodo_test_reach_74941_079a1f28d2429334` — single window.
- `scribodo_test_reach_76826_1a82ad9bf4a7be9b` — multiple windows.
- `build/product-memory-{single,multi}-window-comparison.json` — same-scorer
  comparisons. All reference-budget gates passed; no question exceeded the
  500-ms planning diagnostic in these runs.

The old harness froze its binary and driver/scorer sources, not every
production source. New runs additionally freeze first-party sources and the
registration files; do not retroactively claim that stronger source inventory
for the earlier runs.

## Smaller complete source units — measured, not promoted

The development arm targets **128 measured source tokens**, instead of 512,
using the pinned tokenizer and the existing UTF-8-safe cut path. It retains up
to 16 source units with saved attachments. It is gated behind
`SCRIBODO_MEMORY_SMALL_UNITS=1`, **off by default**. The ordinary 512-token/eight-unit
policy, legacy-unit reads, rendered limits and complete-request recount remain
available unchanged.

Unlike the failed window experiment, this arm supplies complete indexed units,
not request-time cuts selected from their contents. Original revisions remain
intact. Existing indexed projects need an explicit memory Rebuild to regenerate
their units. This does **not** remove the eight-attachment limit or authorize
project-wide retrieval.

Registration: `tests/fixtures/memory_reach/small-unit-registration.json`.
The real-tokenizer/embedder run measured **11/14 exact key passages admitted**,
**12/14 aliases admitted**, **12/14 exact key passages discovered**, and
**24/24 chat tasks with every required source**. Source-range violations and
reference-budget violations were zero. The strict 14/14 target failed, and
discovery lost one designated passage relative to the original. It is not
promoted to the ordinary policy.

The archive grew from **806 to 2,329 units**. Recorded ingestion/indexing time
was **113.067081 s** for the original run and **241.842606 s** for this arm.
These are individual observations, not a performance guarantee. Evidence:
`scribodo_test_reach_85869_5b6911f0be6d610c` and
`build/product-memory-small-unit-comparison.json`.

## Confirmed request/history omission and remediation

The worker previously excluded every message in its bounded host snapshot
from recall, including messages the request builder then removed to fit the
model window. Those messages could therefore be absent from both recent
context and retrieval. Filling the base request before adding recall also left
no reserved capacity for the sources the author enabled memory to recover.

The builder now returns the exact first retained history index, including
skipped-error handling. Recall excludes only the actual retained tail. Local
memory reserves its existing 4,096-token reference allowance before optional
recent history/saved-document heads are selected; the complete request still
must pass the real tokenizer's physical-context check. The newest author turn
is not clipped. Unsaved buffers, edit passages and remote requests retain their
existing foreground policy. No model instructions or inference precision change.

The headless backend also used inconsistent counters: ordinary preparation
estimated tokens while memory admission measured bytes. Its preparation now
uses the same explicit test-only byte protocol; it is not a real-token claim.

A real-SQL, fake-generation regression stores 70 messages, with a fact at
sequence 20 that is copied into the host snapshot but removed from model
context. Before the cutoff/reservation correction it failed **one assertion
in 501 checks**, without a crash or timeout. After the correction **501/501
checks passed**, including retrieval of the exact stored fact. Builder tests
also check the retained index against serialized messages and a skipped-error
boundary, while preserving the newest author's text.

Evidence: `build/product-history-gap-{before,after}.log` and the unchanged
failing control under `build/product-history-gap-control/`. Final broader and
real-model verification must be recorded separately below.

## Request construction under allocation failure

A sweep of every allocation in chat and edit request construction reproduced
**36 partial chat requests** and **34 partial edit requests**. Seven chat
requests lost the author's text; six edit requests lost the selected passage.
The 1,557-check regression failed 70 assertions. These are injected failure
observations, not an estimate of their frequency during ordinary use.

Both builders now check required JSON fields and only serialize complete
requests. Failed, unlinked message objects are freed, and a failed document/text
join rejects the request instead of sending a reduced payload. The tokenizer
request likewise requires its content field. The targeted rerun passed
**1,557 checks with zero partial requests or missing author text**. Logs:
`build/product-request-content-{before,after}.log`; failing control executable:
`build/product-request-control/`. The later full validation is recorded
separately; earlier green runs do not validate this change.

`python3 tools/test_product_memory_mutations.py` checks unchanged controls and
four intentionally weakened guards: retained-history cutoff, recall capacity,
chat publication and edit publication. Only normal assertion failures count as
detecting a mutation; compilation errors, crashes and timeouts do not.

The mutation run completed: **both unchanged controls passed and all four
mutants failed normal assertions**. Restoring the copied-tail cutoff caused
one failure, removing the recall reservation caused two, and bypassing chat
and edit publication guards caused 36 and 34 respectively. No build error,
crash or timeout was counted as a detection. Evidence:
`build/product-memory-mutations/results.json` and its per-variant logs.

## Real-application baseline: a failed gate, not a quality score

The original application was run through the unchanged seven-question script
with seed 42. Its requested 32K conversation context was memory-fitted to 16K.
It finished in **1,063.788 measured process seconds**, with **18 script checks
passing and 10 failing**: all six factual source checks failed; two answer
substring checks passed. Those two matches are not a faithful-use pass. One
matching word occurred in an unrelated answer, illustrating why full outputs
must be reviewed. This run overlapped local verification workloads.

Complete replies are preserved under `build/product-memory-use/before/`.
The diagnostic export initially failed on a MariaDB JSON collation mismatch;
explicit UTF-8 collation recovered all seven complete replies before the owned
test database was removed. The original export error and recovery record remain.

Isolated preparation probes did **not** reproduce that run's missing packet:
the original planner retained seven references in **232.018 ms** while indexing
was warming, and in **228.920 ms** after indexing completed, at a fixed requested
16K context. The complete-index probe checked the opening-scene quotation and
then cancelled generation. It is a preparation test, not an answer-quality run.
Do not claim the separate history-gap correction explains every failure in the
seven-question baseline; its source omission remains a resource/context-dependent
verification gap until a controlled complete-answer rerun establishes otherwise.

Probe evidence: `build/product-memory-use/{prepare-before,ready-before}/`.

The post-fix complete-index preparation probe passed **10 script checks with
zero failures**. It retained seven excerpts, verified `let at last` in the
source packet, and measured **252.222 ms** total preparation with the existing
500-ms deadline unexpired. The trace recorded the 4,096-token reservation and
`packet_prepared=1`. Generation was deliberately cancelled after source
verification: this establishes preparation, not answer correctness. Evidence:
`build/product-memory-use/ready-after/`. The complete-answer run follows
separately with no concurrent heavy validation workload.

## Post-fix complete-answer run: source delivery passes, answer gate fails

The seven-question real-app run completed with **26 script checks passing and
two failing**, exit code 1. All **six factual source checks passed**; four of
six answer-substring checks passed. All seven requests recorded a prepared
packet with seven excerpts, no expired preparation deadline, and preparation
times from **174.000 to 272.377 ms**. The sampled process wall time was
**642.104 s**. These are observations, not an isolated speed comparison with
the original run, whose context selection and competing workload differed.

Full-reply review does **not** establish four faithful answers:

| Question | Complete-reply observation |
|---|---|
| Opening scene / Netherfield | Includes `let at last`, then continues into unrelated prose, fabricated document structure and repetition; hits length limit. Not a faithful-answer pass. |
| Bingley's income | Omits the requested income despite `four or five thousand` in the source packet; degenerates into document-marker repetition; hits length limit. |
| Lady Catherine's gift to Collins | Misidentifies the living as dining invitations/a position at Rosings. |
| Fitzwilliam's age | Answers `about thirty`, with the supporting sentence; includes extraneous material. |
| Georgiana's summer journey | States Ramsgate and two men-servants, but incorrectly identifies the accompanying lady as Lady Catherine. A matching location is not a whole-answer pass. |
| Regiment's encampment | Correctly answers near Brighton, but also presents an altered additional quotation as source text. |
| Darcy's middle name / absence | Does not produce a concise grounded abstention; emits extended search-like narration and false `Edward Darcy` speculation, then hits length limit. |

**Three of seven replies hit the length limit.** The absence question has no
correctness assertion in the existing UI script; its failures above come from
reviewing the complete reply, not from claiming a scripted absence gate.

Evidence: `build/product-memory-use/after/{app.log,messages.json,result.json}`.
All seven generated replies were exported and the owned database was removed
successfully. The source-packet and complete-answer evidence are distinct.
The confirmed host-side fixes and green mechanical tests do not resolve this
observed model-use failure. No prompt/model tuning has been promoted on the
basis of these results, and the complete-product/faithful-answer gate remains
open.

## Mechanical checks so far

After the request-atomicity fix, a serial local rerun passed **all ten targets**:
`all`, `test`, `dbtest`, `sanitize`, `tsan`, `analyzetest`, `analyze`, `uitest`,
`uitest-db` and `offlinetest`. Both unsuccessful retrieval experiment flags were
unset. Evidence: `build/product-atomicity-validation/results.json` and separate
target logs. This includes native UI/store UI and offline acceptance, but is
not a faithful-answer measurement or complete-product certification.

- Application/headless build and tests passed on the candidate.
- The initial DB run stopped at a fixture's old 100-step guard. The smaller
  units require **172 measured steps** for its 20,000-byte source. The fixture
  now requires completed ingestion, the exact preserved byte total and the
  new measured-token bound, rather than stopping with incomplete coverage.
- The targeted document suite passed **3,099 checks**; the complete `dbtest`
  rerun passed. Unicode reconstruction, source/lease fencing and private scope
  checks remain exercised.
- ASan/UBSan passed; local Clang analysis reported zero findings in 83 files.
  These checks preceded the later test-only allocation diagnostic additions;
  final validation must be distinguished from this checkpoint.
- The resource-reporting helper incorrectly labelled real-model runs as
  `fake backend/no LLM`. It now reports the actual selected backend and clearly
  excludes model-server/service RSS from the app-PID sample. Its regression
  test passes. The running original-answer baseline uses the old helper; its
  initial `backend=real-pinned-model` line, not that old parenthetical, identifies
  its backend.
- The audit's intermittent vector-allocation assertion has not reproduced in
  ten instrumented reruns. Actual allocation-call/failure counters and failure
  diagnostics were added; no root cause or production fix is claimed.

## Remaining product scope

The subsequent cross-system investigation is recorded in
[`journal/answer-bedrock_unity.md`](../journal/answer-bedrock_unity.md). It
reproduced another omission: retained LLM history could occupy every SQL/vector
candidate slot before being discarded by the planner's cutoff. Its real
SQL/Qdrant regression changed from **32 candidates / zero admitted sources**
to **two candidates / one admitted original** when both discovery layers
received the cutoff before top-k. This is a specific integration fix, not an
answer-quality certification. The tested alternative embedding instructions
did not improve exact passage admission and remain unpromoted.

`PRODUCT-GAPS.md` remains open: this work does not establish broad model
accuracy, turnkey installation, sustained use, exhaustive interruption recovery,
real macOS input/accessibility or independent author usability. No result here
establishes perfect recall or a complete product.
