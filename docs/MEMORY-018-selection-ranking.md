# MEMORY-018 — Selection ranking: the fusion constant (S3, first change)

**2026-11-08. Host-side selection change for [MEMORY-015](MEMORY-015-effective-memory-contract.md) slice S3, measured before and after on the frozen S2 fixtures.**

**Status:** implemented. One constant in the planner's candidate merge
changed, with the reason measured, a regression test that pins the property,
a mutation that restores the old value and must fail, and the reach harness
rerun on both frozen fixtures. No prompt, model, tier, consent or default
changed. No quality claim about chat answers is made.

## The defect, measured

The planner discovers up to 16 lexical and 16 semantic candidates and merges
them by reciprocal rank fusion: each channel adds `1 / (k + rank)` to a
candidate's score, with `k = 61`, and the eight highest-scoring candidates
are hydrated and admitted. That `k` is the literature's convention for lists
of roughly a thousand. On lists of 16 it flattens rank almost completely:
`1/61 = 0.0164` for rank 1 against `1/76 = 0.0132` for rank 16. Any unit
found by both channels, at any ranks, therefore outscores any unit found by
one channel at rank 1.

From the saved candidate lists of the S2 runs
([MEMORY-017](MEMORY-017-reach-harness.md)), every miss had exactly this
shape:

| Run | Task | Required source | Its ranks (lexical, semantic) | What took the slots |
|---|---|---|---|---|
| 30K | rejected proposal | the author's "No. Not violet. Drop that; ... stays undecided." (59 bytes) | none, **2** | eight filler paragraphs of 478 to 750 bytes at ranks such as (2,10), (15,6), (14,11) |
| 30K | superseded fact | the correction "... Zoë's key is cobalt, not copper." (73 bytes) | none, **2** | filler at (5,3), (4,4), (1,12), (14,10), (11,13), (12,16); the stale original at semantic rank 1 was admitted, the correction was not |
| 100K | superseded fact | the same correction | none, **2** | filler at (12,3), (6,14), (9,15), (15,10), (14,12) |

Two further admitted-but-last cases show the same pressure: at 30K the stated
fact at semantic rank 1 was admitted in the eighth and final slot.

## The constant, measured

Admission was simulated over the saved candidate lists of both runs for a
range of constants. Simulation here means scoring the recorded candidates
with each `k` and taking the top eight; hydration and byte caps did not bind
in these runs.

| k | 30K reach@admitted | misses | 100K reach@admitted | misses | control still 1.0 |
|---:|---:|---:|---:|---:|---|
| 61 (old) | 0.9375 | 2 | 0.9844 | 1 | yes |
| 30 | 0.9375 | 2 | 0.9844 | 1 | yes |
| 15 | 0.9375 | 2 | 0.9844 | 1 | yes |
| 10 | 0.9583 | 1 | 0.9844 | 1 | yes |
| 5 | 1.0000 | 0 | 1.0000 | 0 | yes |
| 3 | 1.0000 | 0 | 1.0000 | 0 | yes |
| 2 | 1.0000 | 0 | 1.0000 | 0 | yes |
| 1 | 1.0000 | 0 | 1.0000 | 0 | yes |

The chosen value is **1**: the convention's 60 scaled to our list length
(60 × 16 / 1000 ≈ 1). It keeps a channel's top result ahead of agreement on
low-ranked candidates, and still rewards agreement near the top (a unit at
rank 1 in both channels scores 2.0; rank 1 in one channel scores 1.0; ranks
2 and 2 score 1.0). Ties keep insertion order, lexical before semantic, as
before. Nothing else in the merge changed.

## Code, tests, mutation

- `src/llm/context_planner.c`: `RECALL_FUSION_K`, with the measurement in the
  comment.
- `tests/test_context_planner.c: test_fusion_rank`: with the fixture's
  lexical list (units 0 to 15) and semantic list (units 8 to 23), the unit
  that is rank 1 in both still comes first, the lexical-only rank 1 unit is
  admitted, and the unit at ranks 16 and 8 is not. The document-mix test now
  asserts that both kinds are admitted rather than their positions, which were
  a ranking artifact.
- `tools/test_memory_pipeline_mutations.py: planner_fusion_scale` restores 61
  and must fail. It does.

## Reach after the change

Both frozen fixtures, same seeds, production ingestion, indexing, planner
and admission, no generation:

| Run | Tasks measured | reach@candidates | reach@admitted before | reach@admitted after | lexical hit | semantic hit | planning p95 | near-misses admitted before → after |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| 30K, 3 per category | 24 | 1.00 | 0.94 | **1.00** | 0.50 | 0.96 | 44 ms | 0 of 6 → 2 of 6 |
| 100K, 4 per category | 32 | 1.00 | 0.98 | **1.00** | 0.50 | 0.91 | 47 ms | 1 of 8 → 2 of 8 |

Every category is at 1.0 in both runs, including the two that missed before
(rejected proposal, superseded fact). The correction turn is now admitted
alongside the original it supersedes. Harness gates all passed. `make test`,
`make dbtest` (including `test_recall` and `test_memory_chat`) and
`make sanitize` pass on the changed planner.

The one movement in the other direction is deliberate to name: the planted
near-miss assertions ("another character's same object is another colour",
"the same character's other object is another colour") are short, specific
author turns of exactly the shape the fix promotes, and two of them now reach
the window in each run where before none or one did. They are relevant
context about the same object, and the model-side question of whether a
near-miss in the window misleads an answer is the kind of thing the use
harness measures, not the reach harness. It is recorded here as a cost, not
hidden in the mean.

## What this does and does not establish

- It fixes the one selection defect the S2 harness exposed, on the fixtures
  that exposed it, with no regression in the categories that were already at
  1.0 and no change to privacy, scope, budget or the rest of admission.
- It does not make lexical discovery useful on common words; that is a
  separate property of the full-text index and is unchanged.
- It is two fixtures and two seeds. The template prose understates real
  distractor pressure. A real manuscript, used locally, remains the stronger
  test, and the harness accepts one.
- Selection reaching the window is still not the model using it. The model
  side is measured separately and its negative result stands.

## What is next

The second S3 candidate from MEMORY-017, correction-aware admission, is not
needed on these fixtures: with the rank fix the correction is admitted
alongside its original. It stays a hypothesis to re-test when a fixture
shows a correction lost for a different reason. The next S3 work is the
lexical channel's behavior on common words, measured the same way, and then
S4.
