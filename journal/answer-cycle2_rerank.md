# Answer cycle 2 — cross-encoder and lexical-overlap arm (predeclared)

**Date:** 2026-11-10. **Question:** the embedder-only comparison (`ri1`) and
its live confirmation showed the query instruction is not the lever for the
three designated units production retrieval misses, and that the stating
spans under 50 characters rank last in their pools under every framing. Does a
ranker that reads question and text together, or plain lexical overlap, place
those units and spans where the embedder cannot?

**Rankers (fixed before the run):**
- `embedder`: Qwen3-Embedding-0.6B, production flags and instruction; the
  `ri1` baseline, recomputed in this run.
- `lexical`: share of the question's distinct content words (function words
  removed, whole-word, case-insensitive) present in the text. Deterministic,
  no model. Tie-break by index.
- `cross`: Qwen3-Reranker-0.6B, Hugging Face revision `e61197ed…`, weights
  hash recorded in the manifest, PyTorch float32 on Metal from
  `build/venv-causal`, scored per the model card (P(yes) over yes/no at the
  last position, the card's prompt and its default instruction), over every
  exported unit (821 per question).
- `cross_top32`: the same reranker over the embedder's top 32 only; units
  outside keep their embedder order below the reranked block. The shape a
  production pipeline would take.

**Inputs (frozen):** the eight questions, the 821 exported units of the
boundary run, the nine designated units from `build/ux4/plan.json`, and the
verified span pools of `build/ux4` (10 expected spans). Output `build/rk1/`.
Tool `tools/answer_rerank_arm.py`; model-free test `tests/test_answer_rerank_arm.py`.

## Measurements

Per designated unit: rank among 821 under each ranker, top-5 and top-16 counts
(16 is the production candidate list size), MRR. Per stating span: rank in its
pool under embedder, lexical and cross. Scores recorded for every designated
unit. Reranker wall time recorded, since cost decides what a pipeline can
afford.

## Predeclared

- The three missed units (three miles for walk and drafts, "not sixteen")
  are the test. Embedder baseline puts them at 31, 24 and 77. A ranker that
  brings any into the top 16 has changed delivery range; into the top 5 is a
  candidate for the production pipeline, to be measured then on the live path.
- Lexical overlap is the control for the hypothesis that the misses are
  vocabulary: the three-miles unit contains "Netherfield" and "Elizabeth"
  (2 of the question's 8 content words). If lexical ranks it high and the
  embedder does not, the embedder is the weak component; if neither does,
  overlap is not enough and the cross-encoder result decides.
- `cross_top32` against `cross`: if the full cross-encoder finds a unit that
  the pipeline shape cannot (because the embedder left it outside its top
  32), recall is the constraint, not reranking.
- Span pools: the four short-span misses (two "only three miles", "twenty
  thousand", "not sixteen") at rank 1 under cross would settle that the span
  ranker should be a cross-encoder; still last would say the span itself is
  the problem and the ranker must see the surrounding unit.
- What would reject the cross-encoder as a component: losing any of the four
  current first places, or wall time that makes 32 pairs per question
  impractical on this Mac.

Results follow below once the run completes; nothing above is edited after it.

## Results, run 1 (`build/rk1/`, all 821 units, 2026-11-10)

A first attempt (`build/rk1-failed/`) exhausted Metal memory because the
scorer materialized logits for every position of every prompt in float32;
the scorer now keeps only the final position (`logits_to_keep=1`), and a
two-prompt smoke test scored a stating sentence 0.9998 against 0.0001 for an
unrelated one. The full run then completed: embedder 94 s, reranker load
1.4 s, reranker scoring 2,016 s for 6,568 question-unit pairs and 29
question-span pairs (about 0.3 s per pair, float32 PyTorch on Metal).

### Designated units among all 821 (rank; lower is better)

| Case | Unit | embedder | lexical | cross | cross_top32 | cross P(yes) |
|---|---|---:|---:|---:|---:|---:|
| single_fact_entail | entail | 4 | 5 | 5 | 5 | 0.999 |
| two_entities_fortunes | hundred thousand | 2 | 2 | 2 | 2 | 0.997 |
| two_entities_fortunes | twenty thousand | 4 | 42 | 10 | 7 | 0.661 |
| single_fact_charlotte_age | twenty-seven | 8 | 28 | 6 | 6 | 0.377 |
| single_fact_walk_distance | three miles | **31** | 11 | **5** | **5** | 0.993 |
| author_correction | girl of fifteen | 8 | 35 | 6 | 6 | 0.967 |
| author_correction | not sixteen | **77** | 130 | **8** | 77 | 0.952 |
| contradictory_drafts | three miles | **24** | 98 | **8** | 7 | 0.908 |
| creative_request | entail | 14 | 34 | 10 | 8 | 0.985 |

| Ranker | First places (of 9) | Top 5 | Top 16 | MRR |
|---|---:|---:|---:|---:|
| embedder | 0 | 3 | 6 | 0.157 |
| lexical | 0 | 2 | 3 | 0.103 |
| cross | 0 | 3 | **9** | 0.187 |
| cross_top32 | 0 | 3 | 8 | 0.184 |

**The cross-encoder puts every designated unit inside the production
candidate size.** The three misses move from 31, 24 and 77 to 5, 8 and 8,
all with P(yes) above 0.9. The pipeline shape (`cross_top32`) keeps two of
the three (the embedder's top 32 contains three miles for both questions)
and cannot reach "not sixteen", which the embedder leaves at 77: for that
unit recall is the constraint, not reranking.

**Lexical overlap is not the answer.** It helps the walk question (31 to 11)
and hurts everything else (twenty thousand 4 to 42, "not sixteen" 77 to
130). The vocabulary hypothesis is wrong as a mechanism: the three-miles unit
shares only 2 of the question's 8 content words, and the units that share
more are about other journeys.

**Why nobody has a first place: the session's own turns.** Under every
ranker the top ranks for the three contested questions are chat units, and
they are the session's own messages: the question itself (lexical overlap
1.0, cross P(yes) 0.9996) and the app's earlier replies. The exported unit
set includes them because the product stores chat as memory. Whether the
current question's own unit is a candidate for itself in production is a
separate question for the retrieval code; the earlier replies certainly
are, which is the assistant-history hygiene item already on the list. The
documents-only view that answers "where does the manuscript unit stand
among manuscript units" was not saved by this run (only each ranker's top 5,
without unit ids), so run 2 below adds it; its bars are the ones already
predeclared, read over document units.

### Stating spans in their pools

| Pool | Span | embedder | lexical | cross |
|---|---|---:|---:|---:|
| entail, fortunes (hundred thousand), Charlotte, fifteen, creative | the four current first places plus fifteen | 1 | 1 | 1 |
| fortunes | twenty thousand | 4 of 5 | **2 of 5** | 4 of 5 |
| walk (added) | only three miles | 3 of 3 | 3 of 3 | 3 of 3 |
| correction (added) | not sixteen | 3 of 3 | 2 of 3 | 2 of 3 |
| drafts (added) | only three miles | 5 of 8 | 7 of 8 | **2 of 8** |

Span first places 6 of 10 for all three rankers. The 44-character "only three
miles. I shall be back by dinner." is last of three under every ranker,
including the cross-encoder that ranks its unit fifth of 821. A span cut from
its context does not carry the question's referents; the ranker has to see
the unit. That settles the pipeline order: rank units, extract from the top
unit, byte-verify, compose.

## Run 2 (predeclared addition, `build/rk2/`)

Same tool, same inputs, same pins; the tool now saves every ranker's full
unit rows and a documents-only view of the same order (chat units removed,
ranks recomputed). The predeclared bars are unchanged and are read on the
documents-only ranks as well as the all-units ranks. Expected if run 1's
reading is right: the cross-encoder's documents-only ranks for the three
misses fall inside the top 5.
