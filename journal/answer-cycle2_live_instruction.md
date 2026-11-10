# Answer cycle 2 — query instruction on the live retrieval path (predeclared)

**Date:** 2026-11-10. **Question:** the embedder-only comparison (`build/ri1`)
found that dropping the production query instruction moved the three missed
designated units from vector ranks 31, 24 and 77 to 15, 15 and 58 and dropped
the creative request's unit from 14 to 94, with no change on the span pools.
Production retrieval is not the vector rank alone (lexical search, fusion and
the context planner follow), so the only honest measurement is old against
new through the real app on the same eight questions. This run is that
measurement. No production source changes; the variant is a diagnostic build.

**Builds (both from `d1134ae`, `tools/answer_live_capture.py`):**
- `old`: `build/answer-live-ri-old/scribodo`, production query instruction
  (binary `3c1d5331…`; contains the production prefix string).
- `new`: `build/answer-live-ri-none/scribodo`, `--query-instruction none`
  (binary `f917f1d5…`; the prefix string is absent). Only `semantic.c`'s
  `k_query_instruction` differs; the vector index fingerprint includes it, so
  the variant indexes its own collection.

**Run:** `tools/answer_product_run.py` with the boundary script
(`tests/ui/store/21_boundary_questions.scribo`), real model and embedder, seed
42, requested 16K context, same real-novel project and chat fixture as
`boundary-1`, live capture on, old first then new, sequentially. Labels
`ri-old` and `ri-none`; captures `build/answer-ri-capture/{old,none}`.

**Comparison:** `tools/answer_live_delivery_compare.py` (test:
`tests/test_answer_live_delivery_compare.py`), per designated passage and per
run: semantic candidate rank, lexical candidate rank, admitted into
`memory_sources`, passage text in the final request; units matched by content
hash. Output `build/answer-ri-capture/compare.json`.

## Predeclared

- Baseline reproducibility: `old` should deliver the same 4 of 7 designated
  passages as `boundary-1` (entail, both fortunes, Charlotte) and miss the
  same three (three miles twice, "not sixteen"). A different delivery set
  under identical inputs is itself a finding and is reported before any
  old-versus-new claim.
- What would make the variant a candidate: `new` admits at least one of the
  three missed passages while keeping the four, with the creative request's
  retrieval not worse in a way that changes its reply class. Admission is the
  bar, not candidate rank; the embedder result already showed candidate rank
  moving without reaching delivery range.
- What would reject it: any of the four current deliveries lost, or the
  creative request losing its sources.
- Replies are recorded (hash, length, truncation) and read, not scored; the
  measurement is source delivery.

Results follow below once both runs complete; nothing above is edited after it.

## Results (2026-11-10)

Both runs completed through the real app: `ri-old` 472 s, `ri-none` 854 s
(the variant's session produced longer replies; wall time is not the
measurement). Script outcome 23 passed, 4 failed in both, the same four
source-delivery expectations as `boundary-1`. Exports: 21 messages each;
403 document units in every run (the chat-unit count varies with the
session's own replies: 821, 817, 818). Databases dropped after export.
Sheet: `build/answer-ri-capture/compare.json`.

### Baseline reproduced

`ri-old` delivered exactly the `boundary-1` set: entail, both fortune units
and Charlotte admitted and in the final request; three miles (twice) and
"not sixteen" absent from both candidate lists; the creative request's entail
unit a semantic candidate at rank 14 of 16 and not admitted. Semantic ranks
identical to `boundary-1` on every designated unit (2; 3 and 1; 3); lexical
ranks within one place. Replies differ from `boundary-1` in every case, as
expected: unit, document and revision identifiers are per import and appear
in the packet headers the model reads.

### Old against new, per designated passage

| Case | Passage | old: semantic / lexical rank, admitted | none: semantic / lexical rank, admitted |
|---|---|---|---|
| single_fact_entail | entail | 2 / 4, **yes** | 3 / 4, **yes** |
| two_entities_fortunes | twenty thousand | 3 / 3, **yes** | 2 / 3, **yes** |
| two_entities_fortunes | hundred thousand | 1 / 5, **yes** | 1 / 5, **yes** |
| single_fact_charlotte_age | twenty-seven | 3 / 16, **yes** | 1 / 15, **yes** |
| single_fact_walk_distance | only three miles | absent / absent, no | **10** / absent, no |
| author_correction | not sixteen | absent / absent, no | absent / absent, no |
| contradictory_drafts | only three miles | absent / absent, no | **10** / absent, no |
| creative_request | entail | 14 / absent, no | **absent** / absent, no |

| | old | none |
|---|---:|---:|
| Designated passages admitted (of 8) | 4 | 4 |
| In the final request | 4 | 4 |
| Among the 16 semantic candidates | 5 | 6 |
| Among the lexical candidates | 4 | 4 |

The live path says what the embedder-only table said. Without the
instruction the three-miles unit enters the semantic candidate list at
rank 10 of 16 for both questions and gets no further: the planner admitted
seven units for the walk question and the tenth candidate was not among
them. "Not sixteen" stays absent. The creative request's entail unit leaves
the candidate list. The four current deliveries are kept.

### Decision

Predeclared bar: admit at least one missed passage while keeping the four.
Not met: zero passages change admission in either direction. The
no-instruction variant is **not** a candidate; the production instruction
stays. The result is filed as the live confirmation of `ri1`: the query
prefix is not the lever for the three missed deliveries, and the lever is
not in the embedder's query side at all. The three misses need a different
retrieval signal (the three-miles unit does not share the question's nouns,
and "not sixteen" ranks 58th at best under any framing), which is the
cross-encoder or lexical-overlap measurement already named.

Harness note: the live-capture build tool now takes `--query-instruction`
and the compare tool reads any number of runs; both are tracked and tested.
Nothing in production changed.
