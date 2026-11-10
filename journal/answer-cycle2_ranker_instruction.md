# Answer cycle 2 — ranker instruction comparison (predeclared)

**Date:** 2026-11-10. **Question:** the ranked-span arm (`build/ux4`) placed the
stating span first in 4 of 5 realistic pools and last or near last for every
span under 50 characters, using the production query instruction, which is
written for statements ("Given a statement from a work of fiction, retrieve
established facts about the same people, places or things"). Does a
question-shaped instruction change the ranks, on the span pools and on the
retrieval side where the same instruction decides which units reach the
request at all?

**Tool:** `tools/answer_ranker_instruction.py` (tests:
`tests/test_answer_ranker_instruction.py`). Qwen3-Embedding-0.6B pinned
`421a27e5…`, production server flags, production 2,000-byte clip and L2
normalization. Spans and units are embedded without an instruction, as
production does. Deterministic; no generative model. Output `build/ri1/`.

**Inputs (frozen):** the verified span pools of `build/ux4` (realistic and
added, 29 spans over 8 cases) and the 821 exported units of the boundary run
(403 document, 418 chat). Designated units per case from `build/ux4/plan.json`.

## Instructions compared (fixed before the run)

| Name | Query prefix |
|---|---|
| `production` | the `k_query_instruction` bytes from `src/llm/semantic.c` |
| `question` | "Instruct: Given a question about a work of fiction, retrieve the passage from the work that states the answer\nQuery: " |
| `qwen_default` | "Instruct: Given a web search query, retrieve relevant passages that answer the query\nQuery: " |
| `none` | no prefix |

## Measurements

1. **Span pools:** rank of each expected-true span in its pool (9 spans; 5
   realistic, 4 added), first places, mean reciprocal rank, and the score
   margin to the best non-designated span.
2. **Retrieval side:** rank of each designated unit among all 821 units and
   among the 403 document units, first places, top-5 count, mean reciprocal
   rank. This is the vector component only; production retrieval also has
   lexical and planner stages, so a unit's rank here is not its delivery.
3. **Determinism:** the `production` arm must reproduce the ux4 span ranks
   exactly, or the comparison is not against the measured baseline.

## What would count

- `question` ahead of `production` on span first places and MRR, and above all
  on the three short-span misses (two "only three miles", "not sixteen"),
  with the four current first places kept.
- On the retrieval side, the designated units that production retrieval
  missed (three miles for walk and drafts, "not sixteen", the entail unit for
  the creative request) moving into the top 5 under `question` would make the
  instruction a candidate for a production change, to be measured then on the
  live retrieval path old against new, not promoted from this table.
- `none` and `qwen_default` are controls: if they match `question`, the gain
  is from dropping the statement framing, not from the new wording.

Results follow below once the run completes; nothing above is edited after it.

## Results (`build/ri1/`, 2026-11-10)

Deterministic: the `production` arm reproduced every ux4 span rank (11 of 11
pools). Correction to the predeclaration: the pools hold **10** expected
spans, not 9 (the fortunes case has two stating spans and the correction case
has two, "girl of fifteen" delivered and "not sixteen" added).

### Span pools: the instruction changes nothing that matters

| Instruction | First places (of 10) | MRR |
|---|---:|---:|
| production | 6 | 0.712 |
| question | 6 | 0.717 |
| qwen_default | 6 | 0.712 |
| none | 6 | 0.712 |

Every one of the four misses stays a miss under every instruction: "only
three miles" last of 3 and 4th or 5th of 8, "fortune of twenty thousand
pounds" 4th of 5, "not sixteen" 3rd of 3. The margins move by hundredths.
The four first places are kept everywhere. The short-span demotion is a
property of the embedding, not of the query framing.

### Retrieval side: rank of the designated unit among all 821 units (and among the 403 document units)

| Case | production | question | qwen_default | none |
|---|---|---|---|---|
| single_fact_entail | 4 (2) | 5 (3) | 4 (2) | 4 (3) |
| two_entities_fortunes, hundred thousand | 2 (1) | 2 (1) | 2 (1) | 2 (1) |
| two_entities_fortunes, twenty thousand | 4 (3) | 4 (3) | 4 (3) | 3 (2) |
| single_fact_charlotte_age | 8 (3) | 6 (2) | 7 (2) | 5 (1) |
| single_fact_walk_distance, three miles | 31 (25) | 17 (12) | 18 (13) | 15 (10) |
| author_correction, girl of fifteen | 8 (4) | 9 (5) | 8 (4) | 5 (1) |
| author_correction, not sixteen | 77 (70) | 76 (69) | 74 (67) | 58 (52) |
| contradictory_drafts, three miles | 24 (18) | 15 (10) | 14 (9) | 15 (10) |
| creative_request, entail unit | 14 (13) | 26 (25) | 29 (28) | **94 (93)** |

| Instruction | Units in top 5 (of 9) | MRR all units | Document-only first places | MRR documents |
|---|---:|---:|---:|---:|
| production | 3 | 0.157 | 1 | 0.289 |
| question | 3 | 0.156 | 1 | 0.289 |
| qwen_default | 3 | 0.160 | 1 | 0.314 |
| none | **5** | **0.183** | **3** | **0.452** |

Dropping the instruction is the only arm that moves the retrieval side, and
it moves it modestly: the three units production retrieval missed go from
31, 24 and 77 to 15, 15 and 58, none into the top 5, while the creative
request's unit falls from 14 to 94 because the instruction is what made that
request behave like a statement about the entail. The question-shaped
instruction is indistinguishable from production on both sides.

## Reading

1. **The ranker instruction is not the lever.** Predeclared target missed on
   every count: no short span recovered, no missed unit into the top 5. The
   `none` control gains on document units but is a wash overall once the
   creative case is counted, and it would have to be measured on the live
   retrieval path before any production claim. No production change proposed.
2. **The failure is length and vocabulary, not framing.** The embedder ranks a
   44-character span below full paragraphs under every framing, and ranks the
   three-miles unit 15th at best because the unit's text does not share the
   question's words ("Netherfield", "walk"). A bi-encoder on whole units
   cannot fix that; a cross-encoder reading question and span together, or an
   extractive reader, is the component to measure next, and lexical overlap
   on the question's nouns is the cheap control to run beside it.
3. **What already works stays:** for the four cases where retrieval delivered
   the stating unit, the embedder ranks that span first under every
   instruction, and the composer answers from one span without fabrication.
   The pipeline from ux4 is unchanged by this result.
