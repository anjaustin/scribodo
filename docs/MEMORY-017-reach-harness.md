# MEMORY-017 — Realistic fixture and the two-metric harness (S2)

**2026-11-08. Implementation and first measurements for [MEMORY-015](MEMORY-015-effective-memory-contract.md) slice S2.**

**Status:** implemented; two reach measurements and one model-side
measurement completed on this Mac. Reach numbers are **measurements of the
current selection path**, not a pass. Nothing here changes production
routing, prompts, tiers, consent or defaults, and nothing here is held-out
evidence.

## Why a new fixture

Every quality experiment before this one planted unique rare-word facts in
filler made of one repeated sentence. Retrieval reached them trivially (the
V2 retrieval arms scored source recall 1.0 while the model scored 1 in 20),
so those runs measured the model and said nothing about selection. The
contract's second metric, *did the right source reach the window*, needs a
corpus where the right source competes with text that shares its words.

## What was built

| Piece | What it does |
|---|---|
| `tools/memory_fixture.py` | Deterministic manuscript-scale author chat. The same ten characters, eight places, ten objects and eight colours recur in every paragraph, so planted items sit among lexical near-misses. Nine labelled categories: `control`, `stated_fact` (with two planted near-miss assertions), `superseded_fact` (fact and later correction), `implied_decision` (assistant proposal, author acceptance turns later), `rejected_proposal`, `protected_ambiguity`, `paraphrase` (question uses words the source lacks), `multilingual`, `absence`. Leak checks: no answer appears in its question or in the 64-turn recent tail. `--export-native` writes the planted turns as episodes for the model-side harness. |
| `tools/memory_reach_eval.c` | Selection-only driver. Real pinned tokenizer (for production token counts), real SQL, real Qdrant and embedder, production ingestion and indexing, then the production planner and admission for every task with **no generation**. The production lexical, semantic and hydration callbacks run unchanged inside recording wrappers, so the driver sees which candidates each path proposed. It never sees required sequences. |
| `tools/memory_reach_eval.py` | Freezes fixture, driver and scorer with hashes; runs; scores per category: `reach_candidates` (required turns among the 32 candidates), `reach_admitted` (among the admitted excerpts), `lexical_hit`, `semantic_hit`, distractors admitted, planning time against the 500 ms budget. Gates only the harness's own validity. |
| `tools/memory_native_eval.py FILE` | The existing model-side harness now takes the exported episodes, so faithful use is measured on the same fixture. |
| `tools/memory_use_score.py` | Coarse alias and openness screen over the native answers, per category, presentation and layout. Not the frozen rubric. |

## Reach measurements

Both runs: production planner and admission, 32 candidates (16 lexical, 16
semantic), 8 admitted excerpts, 4,096 reference tokens, real Qwen3 embedder
and LFM3 tokenizer. Harness gates all passed (control at 1.0, both paths
available, ingestion complete, zero privacy violations, namespaces cleaned).

| Run | Turns | Words | Units | Ingest + index | Tasks | reach@candidates | reach@admitted | lexical hit | semantic hit | planning p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 30K, 3 per category | 403 | 31,745 | 403 | 33 s | 24 measured | 1.00 | 0.94 | 0.50 | 0.96 | 45 ms |
| 100K, 4 per category | 1,116 | 100,097 | 1,116 | 99 s | 32 measured | 1.00 | 0.98 | 0.50 | 0.91 | 47 ms |

Per category at 100K (reach@candidates was 1.0 everywhere):

| Category | reach@admitted | lexical hit | semantic hit |
|---|---:|---:|---:|
| control | 1.00 | 1.00 | 1.00 |
| implied_decision | 1.00 | 1.00 | 1.00 |
| multilingual | 1.00 | 1.00 | 1.00 |
| protected_ambiguity | 1.00 | 1.00 | 0.25 |
| paraphrase | 1.00 | 0.00 | 1.00 |
| rejected_proposal | 1.00 | 0.00 | 1.00 |
| stated_fact | 1.00 | 0.00 | 1.00 |
| superseded_fact | 0.88 | 0.00 | 1.00 |

Planted near-misses admitted: 0 of 6 at 30K, 1 of 8 at 100K.

## What the measurements say

1. **Discovery is not the problem at this scale.** Every required turn was
   among the 32 candidates in both runs. The semantic path carried the realistic
   categories; the lexical path (InnoDB full-text, natural-language mode)
   reached none of the stated, superseded, rejected or paraphrase sources,
   because their words are the corpus's most common words. Lexical discovery
   earns its place on names, numbers and rare tokens, which is what the
   control, multilingual and decision categories contain.
2. **The losses are in admission ranking.** Every miss had the same shape: the
   required turn was a semantic-only candidate, and the eight admitted slots
   went to long filler paragraphs that both paths surfaced. The planner's merge
   ranks candidates found by both paths above candidates found by one, which
   is a reasonable prior that happens to favor long, vocabulary-dense filler
   over a short, specific author turn. In the 100K run the correction turn of
   one superseded fact was displaced this way while its original was admitted,
   which is the worst version of the loss: the window would hold the
   superseded value without the correction.
3. **Budget is not a constraint here.** Planning took about 45 ms at p95
   against the 500 ms production budget, including the embedding call.
4. **Distractor pressure is low but present.** One planted near-miss
   assertion was admitted at 100K. The template corpus is a lower bound on
   real distractor pressure, not an upper bound.

These are the inputs S3 needs. Candidate ranking that weighs role and
specificity (an author's short declarative over a long narrative paragraph),
and correction-aware pairing (admit a fact's later correction with it), are
host-side changes that can be measured against this harness before and after.

## Model-side measurement on the same fixture

The 30K fixture's 24 episodes were exported and run through the unchanged
native harness (`build/mn-mu8px3fo`): pinned model, production sampling at
temperature zero, 8,192-token context, 1,024-token replies, three
presentations and two layouts, 144 answers expected.

**The run did not complete, and the reason was measured, not the one first
recorded.** After 14 answers in 126 seconds of awake time, the compact
presentation of the superseded-fact episode recorded a wall time of
941,070 ms and the driver aborted by design. The first draft of this record
called that a server stall with a slow cancellation. It was neither:

| Evidence | Measurement |
|---|---|
| Power manager log (`pmset -g log`) | `18:28:37 Entering Sleep state due to 'Idle Sleep' ... 942 secs`; `18:44:19 Wake from Deep Idle ... keyboard/UserActivity` |
| Run window | manifest written 18:26:33; events last written 18:44:19 |
| The request | sent 2:03 into the run, at 18:28:36, one second before sleep; the server logged the cancel at 17:44 into the run, at 18:44:20, one second after wake |
| Clocks | the driver's `memory_budget_now_ms` uses `CLOCK_MONOTONIC`, which on macOS keeps counting through sleep (941 s); the Python runner's `time.monotonic()` uses `mach_absolute_time`, which does not (126 s) |
| Why nothing held the machine awake | `pmset -g`: system sleep is 1 minute on AC; the display turned off at 18:25:38; the last `caffeinate` assertion died at 18:28:31; no run of ours held one |
| The same sleep took down CI | the trusted acceptance job on this Mac started its database step at 18:28:28 and its 120-second watchdog, which uses wall-clock `$SECONDS`, fired at 18:44:22, on wake |

Two controlled measurements rule out the alternatives:

- **Replay.** The fourteen frozen requests were replayed in order, twice, against
  a fresh `llama-server` with the production flags. All 28 returned HTTP 200.
  The "stalled" request took 3.93 s and 3.92 s. The longest was 38.2 s for a
  386 KB stream. The server does not hang on this input.
- **Hang probe** (`tools/chat_hang_probe.c`). A real `ChatSession` against a
  stand-in server that accepts the completion and never answers, in two
  modes (no bytes at all; stream headers then silence). In both, cancellation
  requested at 3.000 s left the session idle at 3.030 s with the reply marked
  "Stopped". The worker's cancellation path is sound.

Consequences: the three experiment runners and the acceptance workflow steps
now run under `caffeinate -i`; the native runner also records wall time with
a clock that counts sleep. Separately, because production timeouts and the
500 ms memory budget use `CLOCK_MONOTONIC`, a laptop that sleeps mid-reply
will time out or cancel that reply on wake. That is the behavior one wants
for a connection that is probably dead, but it is a behavior, and it is now
written down.

**The complete run** (`build/mn-zqjcwysh`, under a sleep assertion): 144
answers in 1,670 s with wall and monotonic clocks agreeing; 0 errors, 17
truncated at the 1,024-token allowance; every cohort admitted the same
source IDs across presentations; every current author request intact. The
mechanical gate reports failure only because of the truncations.

Coarse alias and openness screen (`tools/memory_use_score.py`; **not** the
frozen rubric, which requires complete-response review):

| Presentation | Layout | Pass | Fail | Truncated |
|---|---|---:|---:|---:|
| legacy (production) | alternating | 24 | 0 | 0 |
| legacy (production) | consecutive author turns | 0 | 18 | 6 |
| compact | both | 23 | 14 | 11 |
| episode | both | 48 | 0 | 0 |

The legacy split is total and uniform across all eight categories: 3 of 3
pass in the alternating layout, 0 of 3 in the consecutive layout, for every
category including the control. Reading the failing answers confirms what
the scorer counted: with 64 repeated author turns after the sources, the
production presentation makes the model answer about the repetition itself
("The user has repeated the same manuscript note three times", "ordinary
weather at the quay is a placeholder") and ignore the admitted excerpts.
The episode presentation of the same sources and question answers correctly
and cites S1/S2 in all 48 cases, including the superseded fact ("The later
entry supersedes the earlier one ... ochre") and the implied decision.

This is the MEMORY-011 finding reproduced on the realistic fixture with
larger N, which is the S2 gate for the model-side harness. It does **not**
reopen MEMORY-011's no-go: that screen judged speaker attribution,
protected ambiguity, citation fidelity and task form under the rubric, where
episode still failed 6 of 14. What it adds is a clean measurement that the
production presentation loses every recall when the recent history is
consecutive author turns, regardless of category, and that this is a
presentation effect, since the admitted sources were identical.

## Limits, stated plainly

- **The corpus is template prose.** It reproduces vocabulary overlap, not
  the full variety of an author's style. Planted turns are short declaratives
  among long paragraphs, which likely flatters semantic discovery. A real
  manuscript, used locally, would be stronger evidence; the harness accepts
  any archive in the same shape.
- **Chat archive only.** Saved-revision excerpts are a later axis of this
  harness.
- **Candidates are observed at the production limits** (16 per path). The
  harness does not yet report rank positions beyond the cut, so "not a
  candidate" and "ranked 17th" are indistinguishable.
- **Reach is not use.** A source reaching the window is necessary, not
  sufficient. The model-side screen above is coarse and is not the frozen
  rubric; MEMORY-011's negative result stands.
- **Two seeds, two sizes.** Not a distribution. Variance across seeds is
  untested.

## Evidence

Retained under `build/memory-reach/runs/` (manifests, frozen sources, events,
reports) and `build/mn-*` for the model-side run, to be archived per
[EVIDENCE.md](EVIDENCE.md) at the next pass.

## What is next

S3 in MEMORY-015: selection improvements measured against this harness, in
particular admission ranking and correction-aware admission, each as a
before-and-after on the same frozen fixtures. S4 (windowed loading, stable
source identity, latency baseline) can use the same fixture as its workload.
