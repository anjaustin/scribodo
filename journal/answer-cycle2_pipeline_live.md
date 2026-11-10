# Answer cycle 2 — the verified pipeline end to end on the live path (predeclared)

**Date:** 2026-11-10. **Question:** the four measurement cycles decided a
pipeline: a wider first stage, one unit chosen by a cross-encoder, one
extraction call that copies the stating sentence, a byte check of the span
against its unit, and a request carrying only that span before the author's
words. Does it change the eight boundary answers when it runs inside the real
app, with real retrieval, the real model and the real UI script, against the
production path on the same questions?

**What was built (experiment flag, production defaults unchanged):**
- `src/llm/rerank.c`: Qwen3-Reranker-0.6B through llama-server (`docs/MODEL-002-qwen3-reranker.md`;
  pinned BF16 GGUF; parity with the PyTorch reference in `build/rr1`: first
  place agrees 8 of 8, decisions 89 of 89, max difference 0.026, 0.15 s per pair).
- `src/llm/answer_pipeline.c`: 32 lexical plus 32 semantic candidates
  (production's own searches at their maximum limit), hydrated inside the store
  budget; every unit scored; the top unit's stating span extracted by LFM3 under
  a span-only JSON schema at temperature 0; the span verified by the citation
  classifier's normalization (exact, else sentence-boundary prefix, at least 12
  characters); the block "Verified passage from the manuscript, quoted exactly
  (Source 1, part01.md): "…"" or "No verified passage was found…" before the
  author's newest words; the reply's `memory_sources` receipt carries the unit
  id, score, status and span.
- `src/llm/chat.c`: with `SCRIBODO_ANSWER_PIPELINE=verified` (and the memory
  experiment on), the request is persona plus names plus the composer
  instruction ("answer from that passage alone… do not guess"), the full
  history, no attached-document previews, no retrieval packet, no briefs, no
  canon block. The production builder is byte-for-byte the same function with
  the two new inputs empty (`tests/test_answer_pipeline.c`).
- `src/main.c`: the third server is started only under the flag; its memory
  is counted in the context fit.

**Runs:** the same live-capture binary built from this tree
(`build/answer-live-pipeline/scribodo`), the boundary script, real model and
embedder, seed 42, requested 16K context, the same real-novel project and chat
fixture. `old`: flag unset. `new`: `SCRIBODO_ANSWER_PIPELINE=verified`. Old
first, then new, sequentially. Labels `pipe-old`, `pipe-new`; captures
`build/answer-pipe-capture/{old,new}`.

**Comparison:** `tools/answer_live_delivery_compare.py` for delivery (the
new arm's `memory_sources` is the receipt, so "admitted" there means the
verified span's unit is the designated unit and the passage text is inside the
span); `tools/answer_boundary_review.py` for the mechanical reply columns;
the fixture's review fields by hand, case by case, old beside new. Wall time
per question from the trace (`ANSWER_PIPELINE … rerank_ms extract_ms`).

## Predeclared

- `old` must reproduce the known delivery set (4 of 8 designated passages:
  entail, both fortunes, Charlotte) or the comparison stops there.
- The pipeline is judged on the fixture's own terms, field by field. The
  measurements so far predict: entail and Charlotte quoted correctly with a
  possibly muddled gloss; the fortunes answered for one value (the second unit
  ranks fifth or lower); walk and drafts answered "three miles" from a verified
  span if the first stage reaches the unit and the reranker ranks it first
  among candidates (it ranked 1 of 403 document units but chat turns outranked
  it); the correction case quoting fifteen or "not sixteen"; the unavailable
  fact abstaining; the creative request degraded by the composer instruction,
  which is the known cost of having no request classifier.
- Reject the pipeline as a candidate if it fabricates on the unavailable fact,
  guesses a distance where no span verified, or loses more than one of the
  four current correct deliveries. Accept it as the candidate for the product
  design if the correct-answer count rises without a fabrication; everything
  else is a finding to measure next, not a reason to tune a sentence.
- Cost is recorded, not judged: the pipeline runs under its own 90 s deadline
  and is expected to add tens of seconds per question on this Mac.

Results follow below once both runs complete; nothing above is edited after it.

## Results, pass 1 (`pipe-old` / `pipe-new`, 2026-11-10)

Both runs completed through the real app (old 636 s, new 424 s wall; script
23 passed / 4 failed old, 22 / 5 new; 21 messages each; databases dropped).
`old` reproduced the known delivery set (entail, both fortunes, Charlotte
admitted; three miles twice and "not sixteen" absent; the creative unit a
candidate at 14). The pipeline ran on all ten questions: 46 to 63 candidates,
every one hydrated and scored, zero scoring failures, a byte-verified span on
all ten, reranking 10.8 to 15.3 s and extraction 1.3 to 5.3 s per question.

### Selection, with the run 2 cross-encoder scores for attribution

| Case | Top unit (pipeline) | Designated unit | Reading |
|---|---|---|---|
| single_fact_entail | Mrs. Bennet's complaint about the entail (0.9992) | the stating sentence (0.9985) | selection error by 0.0007: "discusses" outscored "states" |
| two_entities_fortunes | **the hundred-thousand unit** (0.9965) | that unit; the twenty-thousand unit (0.661) | one of two values, as predicted |
| single_fact_charlotte_age | **the twenty-seven unit** (0.357) | that unit | correct, at the lowest score of the run |
| single_fact_walk_distance | **the three-miles unit** (0.9928) | that unit | **first delivery of this unit on the live path**, ever |
| author_correction | "My youngest of all is lately married" (0.971) | "girl of fifteen" (0.952), "not sixteen" (0.967) | selection error by 0.004 |
| contradictory_drafts | "how far Netherfield was from Meryton" (0.914) | the three-miles unit (0.908) | selection error by 0.006 |
| unavailable_fact | **the session's own first question** (a chat unit, 0.960) | none | the chat-hygiene failure, exactly as predicted |
| creative_request | an entail unit from part03 (0.996) | the entail sentence (0.985) | a relevant unit; fine for a draft |

Three designated units chosen, three near-misses inside 0.006, one
chat-unit failure. Every verified span was verbatim in its unit; the
"only three miles" span is 16 characters, the fortunes span a 300-character
prefix of its unit.

### Composition: the pipeline's request held the whole chat fixture

| Case | old reply | new reply |
|---|---|---|
| entail | 8,421 chars, copies the attachment preview, never states the entail | 374 chars: "The provided excerpts describe weather events and tides…" **fail** |
| fortunes | both sources quoted, hundred thousand right, sisters wrong (Miss de Bourgh) | weather text, then the composer instruction echoed verbatim **fail** |
| Charlotte | 8,135 chars, correct quote buried, then preview | abstains: "only contains weather observations and tides" **fail** |
| walk | "only twenty-four miles" (wrong journey) | abstains; mentions "a 'three-mile' distance in one recent turn" but will not use it **fail, no guess** |
| correction | **"fifteen"**, from the delivered unit | abstains **fail** |
| drafts | "24 miles" (wrong) | reports the chosen passage gives no mileage; does not merge the draft note **partial** |
| unavailable | fabricates "Edward", then wavers | **fabricates "Bennet"** from the session's own question **fail** |
| creative | draft, marked | draft, marked, quoting the verified span **pass** |

The composer never saw the verified span as the evidence. Measured cause:
the pipeline request is 10 KB where the production request is 38 KB, so the
history budget admitted the entire chat fixture: 65, 63, 58, 55, 51, 49, 45,
39, 28 and 26 messages per request (old: 65, 65, 65, 6, 8, 10, 12, 14, 16, 14;
the packet and previews crowd the fixture out after the third question). The
fixture is sixty "Recent manuscript turn NNN: sleet over the arc…" notes, and
the composer instruction told the model to answer from "the supplied
passages", which it took to be those turns: 4 of 10 replies talk about
weather, 6 of 10 echo "do not guess".

### Decision for pass 1

Rejected as a candidate on the predeclared terms: it fabricated on the
unavailable fact, via the session's own question selected as evidence. Two
structural causes, both measured above and both software:

1. **Chat units are not evidence for the pipeline.** The author's notes are
   already in the history; a chat unit chosen as the "verified passage" is
   the conversation citing itself. Pass 2 restricts candidates to manuscript
   units.
2. **The composer window must be bounded.** With the packet gone, the budget
   fills with history. Pass 2 keeps the newest six messages (three pairs,
   which hold the fixture's own notes typed just before the correction and
   draft questions) plus the verified block.

Pass 2 also logs every scored candidate in the trace so selection errors can
be read from the run instead of reconstructed from `build/rk2`. The
composer wording is not touched: the echo is noted, not tuned.

## Results, pass 2 (`pipe-new2`: manuscript-only candidates, six-message composer window, scored-candidate trace)

Completed through the real app: 399 s wall, script 22 passed / 5 failed, 21
messages, database dropped. The pipeline ran on all ten turns (31 to 55
manuscript candidates scored per question, 8.2 to 15.3 s reranking, 1.4 to
5.5 s extraction; a verified span on every turn). `tools/answer_pipeline_review.py`
joins the trace's scored candidates to each reply
(`build/answer-pipe-capture/review-pipeline-new2.json`); two defects in that
tool were found and fixed on this data (traces paired by position shifted
every row when a turn produced no trace; the same unit chosen twice made the
id match ambiguous). The old arm is `pipe-old`.

### Selection, with every candidate's score now in the trace

| Case | Chosen (score) | Designated unit among candidates | Reading |
|---|---|---|---|
| single_fact_entail | Mrs. Bennet's complaint (0.9992) | rank 3, 0.9985 | selection, by 0.0007 |
| two_entities_fortunes | **hundred-thousand unit** (0.9964) | rank 1; twenty-thousand rank 7, 0.653 | one value, as predicted |
| single_fact_charlotte_age | **twenty-seven unit** (0.357) | rank 1 | correct at the run's lowest score |
| single_fact_walk_distance | **three-miles unit** (0.9927) | rank 1 | correct |
| author_correction | "My youngest of all is lately married" (0.969) | "not sixteen": not a candidate; "girl of fifteen": rank 2, 0.950 | recall for one, selection by 0.019 for the other |
| contradictory_drafts | "how far Netherfield was from Meryton" (0.912) | rank 2, 0.909 | selection, by 0.003 |
| unavailable_fact | "What is his name?" (0.885, sentence-prefix span of 17 chars) | none | the chat unit is gone; an irrelevant manuscript line verifies instead |
| creative_request | an entail unit from part03 (0.996) | rank 9 | relevant for a draft |

Three designated units chosen (fortunes one value, Charlotte, walk); three
selection misses inside 0.02; one first-stage recall miss. The span was
verbatim in its unit in every case, including the 17-character
sentence-prefix cut on the unavailable fact.

### Composition, old beside pass 2

| Case | old (production path) | pass 2 (pipeline) |
|---|---|---|
| entail | 8,421 chars of copied preview; no answer | still answers about "manuscript turn 059…063": the six-message window for the **first** question is the fixture's last five weather notes. **fail** |
| fortunes | hundred thousand right; sisters "Miss de Bourgh" (wrong) | hundred thousand right, quoted; "does not mention the sisters" (**correct for its input**; no stitching) |
| Charlotte | correct quote buried in 8,135 chars of preview | quotes the stating sentence, then "refers to someone else", abstains. **fail by inference**, the fourth time |
| walk | "only twenty-four miles" (fabricated journey) | **"three miles"**, quoted and cited: the first correct live answer to this question |
| correction | **"fifteen"** (correct) | abstains; names the author's correction; no guess. **fail by selection** |
| drafts | "24 miles" (fabricated) | both figures reported with provenance; current passage "gives no mileage"; no merge. **partial, no fabrication** |
| unavailable | fabricates "Edward", then wavers | **abstains**: "the manuscript does not give that detail" |
| creative | draft, marked | draft, marked, built on the verified span |

Fabrications: old 3 (two distances, one first name), pass 2 **0**. Correct
factual answers: old 1 (fifteen), pass 2 1 (three miles) plus one
correct-for-its-input (fortunes). Zero scaffolding markers in any pass 2
reply; the "do not guess" echo persists (it is the composer sentence quoted
back) and is noted, not tuned.

### Decision on the predeclared terms

Not accepted as the candidate: the correct-answer count did not rise (1 to
1), and two of the four current deliveries were lost (entail by a 0.0007
selection margin; the twenty-thousand unit because the pipeline carries one
span). Not rejected either: no fabrication, no guessed distance, and the
unavailable-fact abstention the production path has never produced. The
pipeline changed the failure class from "wrong with confidence" to "silent
or one-sided", which is the trade the P0 asks for, and it did so on the live
path with real retrieval and the real model.

What the two passes measured, in order of leverage:

1. **Selection margins of 0.0007 to 0.02 decide three cases.** The
   cross-encoder cannot tell "states the fact" from "discusses the fact" at
   this resolution. Two spans from the top two units, each byte-verified,
   would carry entail, drafts and the correction, and the fortunes' second
   value, at the cost of the composer seeing two spans (measured safe at two
   in `ux4` when both verified; stitching appeared only at five).
2. **The first question's window is the fixture's tail.** A bounded window
   is right; its content for the first turn is whatever preceded it. The
   composer needs the verified block to outrank history, which a bounded
   window alone does not give; the request order (block before words) is
   the next variable, measured not assumed.
3. **Inference stays the model's ceiling** (Charlotte, four runs).
4. **Recall:** "not sixteen" was outside 64 candidates; the first stage
   still needs the lexical-overlap or document-only search it lacks.

Nothing in production defaults changed; the pipeline remains behind
`SCRIBODO_ANSWER_PIPELINE=verified`.
