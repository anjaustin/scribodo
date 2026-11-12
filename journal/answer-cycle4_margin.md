# Answer cycle 4, the P0 list: the saturation case on two novels

**Date:** 2026-11-11. **Tree:** `02a5b9f` plus the changes recorded below.
**Direction:** "Let's continue with the P0s in order until complete."

## The item

The second novel's one selection miss (journal/answer-cycle4_mars.md, item
4): for "Who is Kantos Kan when the narrator first meets him in the Warhoon
dungeon?" the unit that states what he is ("My fellow prisoner was Kantos
Kan, a padwar (lieutenant) in the navy of Helium") ranks seventh under the
reranker at 0.9913, below six units that name him or the Warhoon prison,
every one within 0.006 of the top (0.9971). The composer, given two passages
that do not say, abstained; the evidence was in the manuscript and was not
delivered. The record called it the sisters' failure in a new form and put it
with MODEL-002.

Cycle 3 measured every selection route for the sisters (arms 1 to 4) and
found one asymmetry it did not act on: the embedder placed the sisters' unit
first where the reranker placed it fifth, and a margin rule, "defer to the
embedder when the reranker's top is not decisive", fitted those seven cases.
It was not proposed on seven cases. With a second novel there are fourteen
cases with sixteen designated units, and the first question of this item is
whether that asymmetry holds on the new book.

## The arm

`tools/answer_margin_arm.py` (test `tests/test_answer_margin_arm.py`, in
`make test`, model-free). It replays recorded live runs rather than
rebuilding pools: the candidate pool and the reranker's score for every unit
are production's own, read from the `ANSWER_PIPELINE_SCORES` lines and
resolved through the run's `units.json`. To each pooled unit it adds the
pinned production embedder's cosine against the question under the
production query instruction (one llama-server session, the pinned GGUF),
and for every unit in the reranker's widest tie it scores every window of
three consecutive sentences with the pinned reranker (cycle 3 arm 2's
granularity) and keeps the best. It then evaluates, over every case of every
run:

- `today`: the two highest reranker scores (`answer_assign_slots`, one part);
- `embedder`: the two highest embedder scores in the pool;
- `margin_M`: the reranker's tie T, every unit within M of the top; if T has
  at most two units, today's slots; otherwise the two highest embedder scores
  within T;
- `fused_M`: the same tie ordered by reciprocal rank fusion of both rankings
  (k = 60);
- `window_M`: the same tie ordered by each unit's best window;

for M in 0.002, 0.005, 0.01, 0.02, 0.03, 0.05, scored as designated units in
slots, with the number of cases whose slots change at all and the cases
gained and lost, so a trade is seen as a trade. One more column is reported
and not ruled on: the designated unit's place, in manuscript order, among
the units that mention the question's longest capitalized phrase.

Calibration: the arm's replay of today's rule reproduces the recorded slots
(the trace line's top and second unit) on 14 of 14 cases.

## Runs

`pipe-window2` (first novel, the eight boundary questions, 7 cases with 8
designated units) and `mars-pipe7` (second novel, 7 cases with 8 designated
units), both at the measured shape: one pool for the whole question,
embedder 32 plus lexical 32, reranker, greedy top two. Output `build/margin3`
(`results.json`, `manifest.json` with the model, binary, fixture, units and
log hashes; `build/margin1` and `margin2` are the development runs of the
same tool and agree on every figure they share). Embedder 105 s over 1,030
document units; reranker 63 s over 700 windows, 0 score failures.

## Result

| Variant | designated in slots, of 16 | cases changed | gained | lost |
|---|---:|---:|---:|---:|
| today | 12 | 0 | 0 | 0 |
| embedder | 6 | 13 | 0 | 6 |
| margin 0.002 to 0.03 | 12 | 1 to 4 | 0 | 0 |
| margin 0.05 | 11 | 7 | 1 | 2 |
| fused 0.002 to 0.03 | 12 | 1 to 4 | 0 | 0 |
| fused 0.05 | 12 | 7 | 1 | 1 |
| window 0.002 to 0.03 | 12 | 2 to 4 | 0 | 0 |
| window 0.05 | 12 | 7 | 1 | 1 |

No rule gains a designated unit without losing one. The four units today
misses are not reachable by any of them:

| Designated unit | reranker rank (score) | embedder rank, pool / all units | best window, rank in tie | first mention |
|---|---:|---:|---:|---:|
| Kantos Kan (second novel) | 7 (0.9913), tie of 9 at 0.01 | **32** / 32 | 10 of 14 (0.9980) | **1** of 21 |
| author correction (second novel) | 4 (0.9517) | 3 / 3 | 1 of 4 (0.9990) | 1 of 81 |
| author correction (first novel) | not a candidate | — / 70 | — | 93 of 176 |
| creative request (first novel) | 23 (0.8678) | 11 / 13 | — | — |

**The cycle 3 asymmetry does not transfer.** The embedder, which saw whose
fortune the sisters' unit stated, places the Kantos Kan unit 32nd of 43 in
the pool and 32nd of 627 document units: the question asks what he is and
the stating sentence is an appositive after his name; the embedder prefers
the arena and escape passages that carry "Warhoon", "prisoner" and his name
at length. The margin rule would have gained nothing here at any margin and
loses two first-novel slots at 0.05.

**Windows do not reach it either.** The stating sentence names him and is
self-contained, so this was the one saturation case where coarse-to-fine
had a chance; the reranker gives its window 0.9980 and gives windows of
nine other tied units 0.998 to 1.000. Within the tie the stating unit's best
window ranks tenth of fourteen.

**The one signal that places it is model-free.** The stating unit is the
first unit in the manuscript that mentions Kantos Kan (1 of 21); the second
novel's correction unit is likewise the first mention of Dejah Thoris (1 of
81). On the first novel the same column is 16 of 176 (entail), 13 and 14 of
154 (fortunes), 10 of 57 (walk and drafts): a first-mention preference would
not have placed any of them, and nothing in the fixtures makes "first" a
property of the stating unit except the Kantos Kan question, which asks for
it in words. A rule written on sight of one case is not proposed. What is
recorded is that a question of the form "who is X" is answered by the
passage that introduces X, that this manuscript introduces X with an
appositive, and that neither pinned ranker prefers an introduction to a
later, longer passage about the same name.

The second novel's correction case changes from a selection miss to a
window result worth noting: within its tie of four at 0.02 the designated
unit's best window ranks first (0.9990), which is the one gain `window_0.05`
and `fused_0.05` report, each paid for with a first-novel loss. The slot that
run gave it already held an equivalent statement of the same fact
(journal/answer-cycle4_mars.md), so nothing was lost to the author there.

## What this closes

The saturation class now has two measured instances on two novels and a
closed table of local routes:

| Route | Where measured | Sisters (first novel) | Kantos Kan (second novel) |
|---|---|---|---|
| Per-part rerank, context parts | cycle 3, arms 1 and 4 | 7 to 5, never a slot | one-part question; not applicable |
| Named-entity admission filter | cycle 3, arm 1 | ranks unchanged | six of the tied units name him |
| Sentence windows | cycle 3, arm 2; here | window rank 18 and 11 | 10 of 14 within the tie |
| Embedder order, margin, fusion | cycle 3, arm 4; here | embedder right, three cases lost | embedder 32nd |
| Larger reranker | MODEL-002 | 8 GB in BF16; not possible on 16 GB | same |

Neither pinned ranker selects the stating unit for either case, and no
deterministic combination of their scores does without a loss elsewhere.
The route that remains for this class is the one the second novel's record
already names: a different model in the reranker role, which the local P0
excludes, or a new signal measured on a third novel before it is written
into the product. The fixture cases stay as permanent regression probes.
MODEL-002 carries the summary under "Saturation".

Gates: `make test` 95,885 C checks and 161 Python tests, 0 failures, exit
status 0 (`build/window/margin-gates-test.log`). No C source changed; the arm
and its test are the only code added, so the analyzer and sanitizer gates
are those of `02a5b9f`.
