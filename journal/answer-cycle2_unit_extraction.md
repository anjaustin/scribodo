# Answer cycle 2 — per-unit extraction arm (predeclared)

**Date:** 2026-11-10. **Question:** does the capability the clean-window review
arm measured (one short fact in one unit against one short claim: contradiction
2 of 2, silence 8 of 8, relevance 0 of 4) hold when the task is the extraction
shape the review-loop design needs: for one unit and the question, copy the
stating sentence or return none? And when a reply is composed from only the
spans that survive a byte check, do the eight boundary cases change?

**Inputs (frozen):** the eight boundary questions, the units production
retrieval actually delivered for each (from the captured requests, verified
against the exported units), and the designated units (`tools/answer_boundary_bind.py`).
Retrieval delivered the designated unit for entail, both fortune units and
Charlotte; it missed the three-miles unit (walk, drafts), the "not sixteen"
unit (correction) and the entail unit for the creative request, which received
no units at all.

**Tool:** `tools/answer_unit_extraction.py` (tests: `tests/test_answer_unit_extraction.py`).
Pinned model and binary by SHA, Metal BF16, `-fa off`, one 8K slot (every call
is under 1,200 prompt tokens), temperature 0, seed 42, thinking off, prompt
cache off, strict JSON schema on extraction. Output `build/ux1/`.

## Stages and arms

| Stage | Window | Calls | Scored against |
|---|---|---|---|
| `extract` | fresh: extractor line, question, one delivered unit | 54 (one per delivered unit) | predeclared truth where known (below) and the byte check |
| `extract_added` | same, for the designated unit retrieval missed | 4 (walk, correction, drafts, creative) | same |
| `compose` | fresh: composer line, question, the byte-verified spans from delivered units, or "No verified passages." | 8 | predeclared reply checks (realistic) |
| `compose2` | identical request | 8 | `compose`: byte-identical or not |
| `compose_added` | as `compose`, spans from delivered plus added units | 4 | predeclared reply checks (added) |

Byte check: a span counts only if the model said the unit answers, the span is
at least 12 characters, and its normalized text (the citation classifier's
normalization) is a substring of the normalized unit. Rejected spans are
counted and never shown to the composer.

## Predeclared truth for extraction

- Designated units answer (`answers: true`, verified span contains the stating text).
- For the correction case, any delivered unit giving Lydia's age ("fifteen" or
  "not sixteen") answers.
- Every other delivered unit is expected not to answer. This is an assumption
  recorded before the run; a unit that turns out to state the answer will be
  inspected and reported, not silently rescored.

## Predeclared reply checks (mechanical, case-insensitive)

| Case | Realistic (`compose`) | With designated unit (`compose_added`) |
|---|---|---|
| single_fact_entail | mentions "distant relation" or "heirs male" | same |
| two_entities_fortunes | mentions "twenty thousand" and "hundred thousand" (attribution read by hand) | same |
| single_fact_charlotte_age | "twenty-seven" | same |
| single_fact_walk_distance | abstention phrase, no "twenty-four" | "three miles", no "twenty-four" |
| author_correction | "fifteen", no "is seventeen" | "not sixteen" or "fifteen", no "is seventeen" |
| contradictory_drafts | abstention phrase, no "twenty-four" | "three miles" and "five", no "twenty-four" |
| unavailable_fact | abstention phrase, no "first name is <Name>" | n/a |
| creative_request | "new writing" or "draft" | same |

Also counted: packet or preview markers in composed replies (expected 0, since
none are in the input), quotation binding of composed replies against a packet
built from the verified spans, and `compose2` identity.

## What would count

- Extraction: every expected-true unit verified (9 of 9 including added; the correction case has a delivered "fifteen" unit as well as the added one) and
  few expected-false units answered. The byte check's rejection count is the
  measurement of how often the model "copies" text that is not there.
- Composition: the realistic arm's cycle 2 tally was 2 passes, 1 correct fact
  buried, 5 failures. A clear majority of the eight checks passing, with zero
  markers and quotations bound, would justify designing the production change
  around this shape. The added arm separates reading from retrieval.
- A failure on the unavailable-fact or absent-source cases rejects the shape on
  the fixture's own terms regardless of the other cases.

Results follow below once the run completes; nothing above is edited after it.

## Results, run 1 (`build/ux1/`, schema order answers then span, 2026-11-10)

82 calls (58 extraction, 24 composition), all `stop`, all extraction replies
parsed against the schema; `compose2` byte-identical to `compose` on 8 of 8.

### Extraction

| | Count |
|---|---:|
| Extraction calls | 58 |
| Model said the unit answers | 4 |
| …span verified in the unit | 2 |
| …span rejected, not in the unit | 1 |
| …span too short | 1 |
| Expected-true units (9) verified | **1** (the added "not sixteen" unit) |
| Expected-false units (49) answered | 3 (one verified: "Miss Elizabeth Bennet." for the fortunes question) |

The boolean failed, not the copying. The schema emitted `answers` before
`span`, so the model committed to false before it had copied anything; then,
in 40 of the 54 false cases, it filled the span anyway. In 25 of those the
span is verbatim in the unit, and for **6 of the 9 expected-true units the
span is the stating text itself** (the entail sentence, "only three miles"
twice, "girl of fifteen", the entail sentence again for the creative case, and
"not sixteen", the one that was also marked true). The byte check would have
accepted all six. The two fortune units and the Charlotte unit returned an
empty span and false: three genuine misses out of nine.

### Composition (realistic: spans from delivered units only)

| Case | Verified spans | Predeclared check | Reply (start) |
|---|---:|---|---|
| single_fact_entail | 0 | fail | abstains |
| two_entities_fortunes | 1 (irrelevant) | fail | abstains, then a stray `</think>` and "Source 1 only mentions 'Miss Elizabeth Bennet.'" |
| single_fact_charlotte_age | 0 | fail | abstains |
| single_fact_walk_distance | 0 | **pass** | abstains, no twenty-four |
| author_correction | 0 | fail | abstains |
| contradictory_drafts | 0 | **pass** | abstains, no twenty-four |
| unavailable_fact | 0 | **pass** | "do not contain any mention of Mr. Bennet's first name" |
| creative_request | 0 | **pass** | two sentences marked "New writing" |

4 of 8 realistic checks pass, 1 of 4 added. Zero packet or preview markers in
any composed reply (cycle 2: 9 of 10 replies carried them). The composer
abstains cleanly whenever it has no spans, including the unavailable-fact
case that cycle 2 answered with a fabrication, and it never guessed a number.
Its defects: the "new writing" clause of the instruction leaks a "New
writing:" line onto factual abstentions; one reply contains a raw `</think>`
with thinking disabled (the product's cleanup would remove it); with the
"not sixteen" span supplied it quoted the span correctly and still called the
age unstated. The quotation count on composed replies is 5 pieces: 1 in a
verified span (cited by label position, not the "Source N" wording), 2 in the
creative draft (new text, correctly unsupported), 2 in the stray fortunes
line.

### Reading of run 1

The four passes are the four cases cycle 2 failed on abstention or form, and
they pass for the right reason: nothing unverified reaches the composer. The
four failures are all upstream, in a boolean the model emitted before reading.
That is a request-construction defect in the diagnostic itself, and it is the
cheapest possible software variable to measure next.

## Run 2 and 3 (predeclared before running)

Same tool, same inputs, same model and seeds; only the extraction schema
changes. `--variant span_first` emits the span before the boolean.
`--variant span_only` drops the boolean; an empty span means none. Outputs
`build/ux2/` and `build/ux3/`. Expected if the field-order reading is right:
expected-true verified rises from 1 toward the 6 whose spans were already
correct; the three empty-span misses may or may not move. The figure to watch
on the other side is expected-false units with a *verified* span, which the
byte check cannot catch: that is the relevance error rate of this shape, and
it decides whether the composer can be trusted with everything that verifies.

## Results, runs 2 and 3 (`build/ux2/` span then boolean, `build/ux3/` span only)

All 82 calls per run finished with `stop`; every extraction reply parsed;
`compose2` byte-identical to `compose` on 8 of 8 in both runs. Model time
151 s, 180 s and 208 s for runs 1 to 3.

### Extraction across the three schemas

| | run 1: boolean, span | run 2: span, boolean | run 3: span only |
|---|---:|---:|---:|
| Units where the model said "answers" | 4 | 6 | 58 (every unit) |
| Spans verified in the unit | 2 | 5 | 42 |
| Spans rejected by the byte check | 1 | 1 | 16 |
| Expected-true units verified (of 9) | 1 | 2 | **8** |
| Expected-false units with a verified span (of 49) | 1 | 3 | **34** |

**The boolean is broken in either order.** With the span emitted first, the
model copied the entail sentence, the "twenty-seven" sentence, "girl of
fifteen" and "not sixteen" and then wrote `answers: false` under each of
them. Reordering moved the count from 1 to 2. The relevance judgment is not
coming from the copied text at all.

**Span only has recall and no precision.** Asked only to copy the stating
sentence or return empty, the model never returned empty: 58 of 58 spans were
non-empty. The byte check removed 16 that were not in the unit. Of the 42
that survived, 8 are the stating text for the 8 expected-true units it should
have found, and 34 are verbatim sentences from units that do not answer the
question ("I shall depend on hearing from you very often, Eliza." for
Charlotte's age; "_Boulanger_" for Mr. Bennet's first name). The byte check
cannot see relevance, by design, so all 34 reached the composer.

The one expected-true miss in run 3 is a harness artifact: the "girl of
fifteen" span is 599 correct characters followed by the schema's 600-character
cap, and the final forced character made the byte check fail. The three
genuine misses in runs 1 and 2 (both fortune units and Charlotte returned an
empty span) disappear in run 3.

### Composition

| Case | run 1 | run 2 | run 3 (spans given) | run 3 by hand |
|---|---|---|---|---|
| single_fact_entail | fail (0 spans) | fail (0) | **pass** (6) | Quotes the entail sentence verbatim, cites it; the explanation then inverts it ("passed to a distant relative rather than to male heirs") and adds a false aside about a son from another span. Correct quotation, wrong gloss. |
| two_entities_fortunes | fail (1) | fail (1) | **pass** (7) | Both figures quoted verbatim from the right units and attributed to the right parties; one invented link ("Mrs. Jenkinson's four nieces—Bingley's sisters") stitched from an irrelevant span. |
| single_fact_charlotte_age | fail (0) | fail (0) | **pass** (4) | Quotes the right sentence, then says it "refers to Elizabeth, not Charlotte" and abstains. Mechanical pass on the word, substantive **fail**. |
| single_fact_walk_distance | pass (0) | pass (0) | pass (6) | Abstains correctly with six irrelevant spans. Added arm: "only three miles", correct, in both runs 2 and 3. |
| author_correction | fail (0) | fail (1) | fail (3) | Abstains; the "fifteen" span was lost to the cap artifact. Added arm fails in both runs: with "not sixteen" in hand it says the age is not given. |
| contradictory_drafts | pass (0) | pass (1) | pass (8) | Abstains correctly with eight irrelevant spans. Added arm: run 2 names "nearly fifty miles" as the current figure from an irrelevant span; run 3 abstains with nine spans despite "only three miles" among them. **Fails both ways.** |
| unavailable_fact | pass (0) | pass (0) | pass (4) | Abstains cleanly in all three runs; never names a first name. |
| creative_request | pass | pass | pass | Two sentences marked as new writing in all runs; with the entail span supplied, the draft follows the source. |

Mechanical tally: 4, 4 and 7 of 8 realistic; 1, 2 and 2 of 4 added. Zero
packet or preview markers in 36 composed replies. Composed quotations over
run 3: 11 pieces, 2 bound to the cited label, 5 in another supplied span, 4
in no supplied text (the two creative sentences and two paraphrases).
"New writing:" leaked onto 7, 2 and 2 factual abstentions.

### Reading

1. **Structural wins that held in every run:** no fabricated answer on the
   unavailable fact, no guessed distance when the source was absent, no
   scaffolding echoed, byte-identical replies on repeat. These are the cycle 2
   failures that were about what reached the model, and they are fixed by
   what reaches the model.
2. **The model cannot judge relevance for one unit either.** Boolean 1 or 2
   of 9 in either field order; span-only copies something from every unit.
   This is the same finding as the review arm (0 of 4 answering passages
   recognised), now at the smallest possible task. Selection cannot be
   delegated to this model at any granularity measured so far.
3. **Composition over verified spans is better than composition over the
   packet, and still misreads.** With the stating sentence among 4 to 7
   spans it quoted it correctly three times out of three, then glossed it
   wrongly once, denied it once, and got it right once. With 9 spans it lost
   it. Fewer, more relevant spans is the only direction the numbers support.
4. **Therefore the specialist is not optional.** The structural gate needs a
   relevance ranker that is not this model: the embedder already running, a
   cross-encoder, or an extractive reader that returns positions. The next
   measurement is a ranked-span arm: score each verified span against the
   question with Qwen3-Embedding, keep the top one or two, and compose. If
   that recovers entail, fortunes and Charlotte without losing the
   abstentions, the production design follows.

Harness items to fix before the next arm: lift the 600-character span cap
(cut in software at a sentence boundary instead), and separate the creative
instruction from the factual composer so "New writing:" cannot leak.
