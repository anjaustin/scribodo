# MEMORY-018 — Selection ranking and lexical discovery (S3)

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

## Second change: the lexical channel

### The defect, measured

After the fusion fix, both reach runs still showed the lexical channel
reaching **none** of the stated, superseded, rejected or paraphrase sources
(lexical hit 0.50 overall, entirely from the control, decision, multilingual
and ambiguity categories whose turns contain rare tokens). `tools/lexical_bench.py`
isolates the channel: it loads a frozen fixture's pre-boundary archive into a
scratch table with the production index (InnoDB full-text, `utf8mb4_bin`),
runs every task's question, and reports where the required sources rank
under the production query and alternatives. No planner, vectors or model.

InnoDB's natural-language score sums term frequency with no length
normalization, so a 700-byte paragraph that mentions the query's common words
three times outranks the 57-byte author turn that states the fact once.
Boolean mode shares the defect. Dividing by a power of the text length
removes it:

| Score | 30K lexical hit | 30K worst rank | 100K lexical hit | 100K worst rank |
|---|---:|---:|---:|---:|
| natural (production) | 0.500 | 2 | 0.500 | 3 |
| natural / log(len) | 0.688 | 14 | 0.500 | 4 |
| natural / sqrt(len) | 0.875 | 3 | 0.875 | 5 |
| natural / len^0.6 | 1.000 | 12 | 0.906 | 15 |
| **natural / len^0.75** | **1.000** | **6** | **1.000** | **8** |
| natural / len^0.9 | 1.000 | 6 | 1.000 | 8 |
| natural / len | 1.000 | 6 | 1.000 | 8 |
| boolean OR | 0.500 | 2 | 0.500 | 3 |

"Hit" is the share of required sources inside the channel's 16 candidates;
"worst rank" is the deepest position of a found source. The exponent is
**0.75**: the start of the plateau where every lexically reachable source is
found on both fixtures, with the best worst-case rank. Normalization is not
unbounded: at this exponent a 1,000-character paragraph must repeat a query
word about eleven times to outrank a one-sentence statement of it, which the
database test demonstrates with a realistic paragraph (three mentions) and
documents as the limit. The paraphrase sources are reached through the
character's name alone, which is why they appear here but not in a question
without a name.

`MEMORY_LEXICAL_LENGTH_NORM` in `memory_store.h` carries the constant into the
chat and document candidate queries and the recall view's document excerpts.

### A second defect the bench exposed: case

An earlier run of the bench disagreed with production by exactly the
name-only categories. The cause was measured, not inferred: that run called
`OPTIMIZE TABLE` after loading, which rebuilds the full-text index, and under
`utf8mb4_bin` InnoDB's full-text index is **case-sensitive and inconsistent
across build paths**:

| Index built by | `Tarn` | `tarn` | `TARN` |
|---|---:|---:|---:|
| incremental inserts (production today) | 175 | 0 | 0 |
| `ALTER TABLE ... MODIFY` on another column | 175 | 0 | 0 |
| `DROP INDEX` + `ADD FULLTEXT` | 0 | 175 | 0 |
| `OPTIMIZE TABLE` (recreate) | 0 | 175 | 0 |
| any of the above, `utf8mb4_unicode_ci` | 175 | 175 | 175 |

(175 of 339 rows contain "Tarn".) So in production today an author who types
"tarn" or "AMBER" finds nothing lexically, and any future index rebuild would
flip which case works. Migration 014 changes `memory_units.text` to
`utf8mb4_unicode_ci`. Measured on a populated binary table: the `ALTER`
took 0.08 s for 339 rows, every case matched afterwards, rows inserted after
the change match in every case, a later `OPTIMIZE` keeps them consistent, and
hashes and binary comparisons of the text are unaffected. The collation is
also accent-insensitive ("Zoe" finds "Zoë"), which is acceptable for a
discovery channel whose every candidate is verified byte-for-byte before
admission. The bench no longer rebuilds the index and takes `--collation`.

### Tests and mutations

- `test_recall: lexical length normalization`: a one-sentence fact outranks a
  realistic paragraph that mentions the query's words three times; an
  upper-case query and a lower-case name both find it.
- `tools/test_recall_mutations.py`: `lexical_length_norm` removes the
  normalization and `lexical_case_fold` restores `utf8mb4_bin` in migration
  014; both are killed. `test_db` asserts schema version 14.

### Reach after both lexical changes

| Run | reach@candidates | reach@admitted | lexical hit before → after | semantic hit | planning p95 | near-misses admitted |
|---|---:|---:|---:|---:|---:|---|
| 30K, 3 per category | 1.00 | 1.00 | 0.50 → **1.00** | 0.96 | 47 ms | 5 of 6 |
| 100K, 4 per category | 1.00 | 1.00 | 0.50 → **1.00** | 0.91 | 49 ms | 5 of 8 |

Every category reaches 1.0 through both channels now, including paraphrase,
which lexical reaches through the character's name. The lexical channel is
no longer a dead path on common words, so a weaker semantic result on a real
manuscript has a second route to the source. Near-miss admission rose again
(they are short and specific, exactly what both fixes promote); the
model-side measurement below is why that is recorded as a cost in window
space rather than in correctness. `make test`, `make dbtest`, `make sanitize`
and `make tsan` pass on the final state.

## The near-miss cost, measured on the model side

The ranking changes admit more of the planted near-miss assertions (short,
specific turns about the same object with another character or colour). To
measure whether a near-miss in the window misleads an answer, the seven
stated-fact episodes from both fixtures were run through the unchanged native
harness twice: with the fact alone in the episode, and with both planted
near-misses around it. 42 answers each, three presentations, two layouts,
same model, same settings, both under a sleep assertion (wall 532 s and
554 s, no sleep).

| | Correct colour | Wrong colour asserted | Correct and also names the near-miss |
|---|---:|---:|---:|
| without near-misses | 21 of 42 | 0 | 0 |
| with near-misses | 21 of 42 | 0 | 7 |

The 21 correct answers in each run are exactly the alternating-layout
answers plus the episode presentation's consecutive-layout answers; the
production presentation's consecutive-layout answers fail in both runs for
the reason MEMORY-017 measured, unrelated to near-misses. Not one answer in
either run asserted a near-miss colour as the fact. In seven answers the
model named the near-miss alongside the correct value ("Tarn's coat is
violet; Dvořák's coat is amber"), which is accurate disambiguation of the
window's contents, not an error. Conclusion for this fixture: admitting
near-misses costs window space and some verbosity, and no correctness. The
cost column in the fusion table above can be read with that in hand.

## What is next

The second S3 candidate from MEMORY-017, correction-aware admission, is not
needed on these fixtures: with the rank fix the correction is admitted
alongside its original. It stays a hypothesis to re-test when a fixture
shows a correction lost for a different reason. The next S3 work is the
lexical channel's behavior on common words, measured the same way, and then
S4.
