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

## Pass 3 (predeclared): two verified spans, and the block ahead of history

**Change under test (one at a time, old = pass 2):**
- **3a, two spans.** The pipeline keeps the top two manuscript units by
  reranker score, extracts from each, byte-verifies each, and the block lists
  the verified spans as Source 1 and Source 2 in score order. A unit whose
  span fails verification contributes nothing; the block says how many spans
  verified. Decided from the pass 2 margins (0.0007, 0.003, 0.019 between
  the chosen unit and the designated one) and from `ux4`, where two verified
  spans composed safely and stitching appeared only at five.
- **3b, block ahead of history.** Same as 3a, with the verified block placed
  in the system message after the composer instruction instead of at the top
  of the author's newest message, so it precedes the bounded history rather
  than following it. Diagnostic switch `SCRIBODO_ANSWER_BLOCK=system`.

**Runs:** `pipe-new3a`, `pipe-new3b`, same binary lineage, script, seed and
fixture as passes 1 and 2; captures `build/answer-pipe-capture/{new3a,new3b}`;
sheets from `tools/answer_pipeline_review.py` (which now reads up to two
chosen units per receipt).

**Predeclared:** the four current deliveries should come back with two spans
(entail at rank 3 on pass 2 is the one at risk); the correction case should
carry "girl of fifteen" (rank 2); drafts should carry three miles (rank 2);
the fortunes should carry both values only if the twenty-thousand unit rises
from rank 7, which two spans do not promise. Reject either pass on a
fabrication or a guessed distance. Accept 3a as the candidate if the
correct-answer count rises above pass 2's one without a fabrication; compare
3b to 3a on the first question (the fixture-tail window) and on any case
where the composer ignored a verified span in favor of history.

Results follow below once both runs complete; nothing above is edited after it.

## Results, pass 3 (`pipe-new3a` two spans; `pipe-new3b` two spans with the block in the system message)

Both completed through the real app (420 s each; script 23 passed / 4
failed, the same four source-delivery expectations as every run; databases
dropped). Selection is identical in 3a and 3b, as it must be: the same
binary, seed and candidates. Reranking 10.2 and 12.6 s mean, extraction 4.7
and 4.9 s mean per question (two calls now). Sheets
`build/answer-pipe-capture/review-pipeline-new3{a,b}.json`; the review tool
reads the second unit from the receipt and the trace.

### Selection with two units

| Case | First (score) | Second (score) | Designated carried |
|---|---|---|---|
| single_fact_entail | complaint, part02 (0.9992) | "I cannot bear… the entail", part03 (0.9986, prefix) | no: the stating sentence is rank 3 |
| two_entities_fortunes | hundred thousand (0.9964) | Miss de Bourgh's fortune, part02 (0.65) | one of two; the sisters' unit stays rank 7 |
| single_fact_charlotte_age | twenty-seven unit (0.357) | "at the age of twenty-seven", part03 (0.18): Charlotte's own reflection | **both relevant** |
| single_fact_walk_distance | "only three miles" (0.9927) | "walked three miles… in such dirty weather" (0.99) | both relevant |
| author_correction | "lately married" (0.969) | "Lydia, my love, though you are the youngest" (prefix) | no: "girl of fifteen" was not among this run's top two |
| contradictory_drafts | Meryton distance (0.912) | **"only three miles. I shall be back by dinner."** (0.909) | **yes, as second** |
| unavailable_fact | "What is his name?" (0.885, prefix) | "Mr. Bennet still up." (0.54) | nothing relevant exists; two verbatim fragments verified anyway |
| creative_request | part03 entail (0.996) | part02 entail complaint (prefix) | relevant for a draft |

### Composition, three passes side by side

| Case | pass 2 (one span) | 3a (two spans, block in message) | 3b (two spans, block in system) |
|---|---|---|---|
| entail | weather confusion | weather confusion | **honest abstention**: "surrounding context… cut off; cannot confirm" |
| fortunes | hundred thousand; "does not mention the sisters" | hundred thousand; **Miss de Bourgh offered as the sisters' fortune** | same misattribution |
| Charlotte | abstains | abstains ("need a birth date") | **"twenty-seven when the story opens"**, from Source 2, the first correct Charlotte answer in six runs |
| walk | **three miles** | **three miles** | **three miles** |
| correction | abstains | abstains; identifies Lydia as the youngest | abstains; identifies Lydia as the youngest |
| drafts | "gives no mileage" | **three miles vs five miles**, both with provenance | **three miles vs five miles** |
| unavailable | abstains | **"first name: Bennet"** | **"first name is Bennet"** |
| creative | draft, marked | draft, marked | draft, marked |

| | production | pass 2 | 3a | 3b |
|---|---:|---:|---:|---:|
| Correct factual answers | 1 | 1 | 2 | **3** |
| Fabrications | 3 | 0 | 1 | 1 |
| Misattributions | 1 | 0 | 1 | 1 |

### Decision on the predeclared terms

Both passes are **rejected** by the rule written before the run: a
fabrication on the unavailable fact. The second span did it. With one
verbatim but irrelevant fragment ("What is his name?") the composer
abstained on pass 2; with two ("Mr. Bennet still up." added, score 0.54) it
answered "Bennet". Verification guarantees the words exist in the
manuscript; it says nothing about whether they bear on the question, and
two fragments gave the model enough to infer a first name from a surname.
The fortunes misattribution is the same mechanism: a verified second span
about Miss de Bourgh, offered as the sisters' fortune.

What the passes established, in order of leverage:

1. **The block belongs ahead of history.** 3b against 3a, selection
   identical: the first-question confusion is gone and Charlotte is answered
   from the second span. The system-message placement is the better
   request shape and carries no cost measured here.
2. **Two spans raise recall and remove the abstention safety.** 1 → 3
   correct, 0 → 1 fabrications, 0 → 1 misattributions. The composer uses
   every span it is given; relevance has to be decided before it sees them.
3. **The reranker's score does not separate relevant from irrelevant spans.**
   Charlotte's correct units scored 0.357 and 0.18; the unavailable fact's
   irrelevant fragments 0.885 and 0.54. No threshold on this score keeps the
   right ones and drops the wrong ones. The relevance gate is still the open
   problem, and it is a software problem: a verified span answers the
   question only if the composed answer can be found inside it, which is a
   check the pipeline can make on the reply before it is shown.
4. Entail (rank 3) and the sisters' unit (rank 7) are outside two slots;
   "not sixteen" outside 64 candidates.

Nothing in production defaults changed. The pipeline remains behind
`SCRIBODO_ANSWER_PIPELINE=verified`; `SCRIBODO_ANSWER_BLOCK=system` selects
the 3b placement.

## Pass 4 (predeclared): the answer-in-span gate

**Change under test (old = pass 3b):** after the reply streams, a second
clean-window call asks the model only what answer the reply gives (schema
`{"answer": string}`, empty when the reply abstains). Software then decides:
- `no_answer`: the reply abstained; shown as is.
- `pass`: every part of the answer (split on `;` and `,`) is, after the
  citation classifier's normalization, inside a verified span or inside one
  of the author's own turns in the composer window (the author's draft note
  is a legitimate source for the author's figure), and the answer is not
  merely inside the question.
- `echoes_question`: the answer is inside the question itself ("Bennet" for
  "Mr. Bennet's first name"): an echo carries no information from the
  manuscript.
- `not_in_span`: a part of the answer is in neither a verified span nor the
  author's turns.
On `echoes_question` or `not_in_span` the shown reply is replaced by a
withheld notice that lists the verified passages; the receipt records the
withheld answer and the verdict. The gate applies to requests that end in a
question mark, a stand-in for the request classifier the product does not
yet have; the creative request is therefore ungated and its result is the
measured cost of that stand-in.

The verified block stays in the system message (pass 3b's placement is now
the default; `SCRIBODO_ANSWER_BLOCK=message` restores the pass 3a shape).
Two spans as in pass 3. Model calls per turn: rerank, two extractions, the
reply, the claim call.

**Predeclared:** the unavailable fact's "Bennet" is withheld as
`echoes_question`; the fortunes' Miss de Bourgh misattribution is **not**
caught by this gate (the quoted figure is in a verified span; the error is
in the relation, which this gate does not read) and stays as the next
problem; walk, drafts and Charlotte pass through unchanged
(`three miles`, `twenty-seven` are in spans; `five miles` is in the author's
note). Reject the pass if the gate withholds any of the three correct
answers. Accept it as the candidate if fabrications return to 0 with the
correct count held at 3.

Results follow below once the run completes; nothing above is edited after it.

## Results, pass 4 (`pipe-new4`: the answer-in-span gate, phrase containment)

Completed through the real app (script 23 passed / 4 failed; the gate ran on
the seven questions, 0.7 to 2.2 s each; the creative request was ungated by
the question-mark rule). Pre-gate replies recovered from the live capture;
sheet `build/answer-pipe-capture/review-pipeline-new4.json`.

| Case | Pre-gate reply | Claim the model reported | Verdict | Shown |
|---|---|---|---|---|
| entail | abstains | "The supplied manuscript passages do not provide enough detail…" | not_in_span | withheld notice |
| fortunes | hundred thousand right; Miss de Bourgh offered as the sisters' | "Mr. Bingley inherited nearly a hundred thousand pounds; his own inheritance is said to be uncertain…" | not_in_span | withheld notice |
| Charlotte | abstains, **imitating the withheld notice from earlier turns** | "…is not mentioned in the supplied text." | not_in_span | withheld notice |
| walk | three miles, quoted | "three miles" | **pass** | three miles |
| correction | abstains | "The supplied manuscript passages do not give the age…" | not_in_span | withheld notice |
| drafts | **three miles vs five miles, correct** | "3 miles; 5 miles" | not_in_span | **withheld** |
| unavailable | fabricates "Bennet" | "Bennet" | **echoes_question** | **withheld** |
| creative | draft, marked | ungated | | draft |

Fabrications 0: the gate caught "Bennet" exactly as predeclared. But it also
withheld a correct answer, so the pass is **rejected** by its own rule. Three
mechanical causes, all in the gate, none in the pipeline:

1. **Abstentions are reported as sentences.** Asked for the answer "or an
   empty string if the reply gives no answer", the model returned the
   abstention in prose four times out of four; phrase containment then
   called a correct abstention `not_in_span`. The shown text was still an
   abstention, so no reader was misled, but the verdict was wrong.
2. **Numerals.** "3 miles; 5 miles" against "three miles" and "five miles":
   the normalization folds quotes, dashes and emphasis, not number forms.
3. **Phrase containment is paraphrase-blind.** "inherited nearly a hundred
   thousand pounds" is not a substring of "inherited property to the amount
   of nearly a hundred thousand pounds", though every number and name in it
   is.

One side effect to record: once a withheld notice is stored as the reply, it
is in the next turn's history, and the composer imitated it on the Charlotte
turn ("An answer was withheld because…" written by the model). The run's
Charlotte answer from pass 3b did not recur under this history.

## Pass 5 (predeclared): the anchor-token gate

Same pipeline, same claim call; the decision changes from phrase containment
to anchors:
- A claim that matches the abstention vocabulary ("do not give", "not
  mentioned", "not stated", "not provide", "no information", "cannot answer")
  is `no_answer`, shown as is.
- Otherwise the claim's **anchor tokens** decide: every number (digits or
  number words, canonicalized both ways) and every capitalized word in the
  claim must occur in a verified span or the author's own turns in the
  window; a claim whose anchors are all inside the question is
  `echoes_question`; a claim with no anchors at all is `pass` (nothing to
  check; recorded as `pass_no_anchors`).
- The withheld notice is unchanged.

**Predeclared:** walk `pass`, drafts `pass` (3 and 5 canonicalize to the
span and the author's note), the Bennet echo still withheld, abstentions
`no_answer`, the fortunes misattribution still `pass` (Miss de Bourgh is in
a span; the relation error is beyond this gate). Reject if any correct
answer is withheld; accept as the candidate if fabrications stay at 0 and
the correct count returns to the pass 3b level. Results follow below.

## Results, pass 5 (`pipe-new5`: the anchor-token gate)

Completed through the real app (script 23 passed / 4 failed; gate 0.6 to
2.6 s per question). Pre-gate replies from the capture; sheet
`build/answer-pipe-capture/review-pipeline-new5.json`.

| Case | Pre-gate reply | Claim | Verdict | Shown |
|---|---|---|---|---|
| entail | abstains | "…do not provide enough detail…" | **no_answer** | abstention |
| fortunes | hundred thousand right; Miss de Bourgh as the sisters' | "…nearly a hundred thousand pounds…; Miss de Bourgh… a very large fortune…" | pass | reply (misattribution, as predeclared) |
| Charlotte | **"twenty-seven when the story opens"**, from Source 2 | "27" | **not_in_span** | **withheld** |
| walk | three miles, quoted | "only three miles" | pass | three miles |
| correction | abstains | "" | **no_answer** | abstention |
| drafts | three vs five, both with provenance | "three miles; five miles" | pass | **correct** |
| unavailable | **"first name is Bennet… Edward Bennet"** | "Bennet" | **echoes_question** | **withheld** |
| creative | draft, marked | ungated | | draft |

Fabrications 0 for the second pass running: the gate withheld the "Bennet"
echo again, this time over a pre-gate reply that had also invented "Edward".
Abstentions now read as `no_answer` (two of two). Numerals in simple form
work ("three miles; five miles" passed). But Charlotte's correct answer was
withheld, so the pass is **rejected** by its rule, and the cause is one
line: the number canonicalization covered 0 to 20 and the tens, and the
claim was "27" against a span saying "twenty-seven". The predeclared design
said "number words canonicalized both ways"; the implementation did not
cover compounds. A test gap, closed below, not a design change.

## Pass 6 (predeclared): the same gate with compound numbers

Only the canonicalization changes: 21 to 99 in both directions
("twenty-seven", "twenty seven", "27"), with a test for each form. Same bars
as pass 5. Results follow below.

## Results, pass 6 (`pipe-new6`: the anchor-token gate with compound numbers)

Completed through the real app (script 23 passed / 4 failed; gate 0.6 to
2.5 s per question). Sheet `build/answer-pipe-capture/review-pipeline-new6.json`;
pre-gate replies from the capture.

| Case | Pre-gate reply | Claim | Verdict | Shown |
|---|---|---|---|---|
| entail | abstains | "…do not provide enough detail…" | no_answer | abstention |
| fortunes | hundred thousand right; Miss de Bourgh as the sisters' | "…nearly a hundred thousand pounds…; Miss de Bourgh…" | pass | reply, with the misattribution |
| Charlotte | **"twenty-seven when the story opens"** | "27" | **pass** | **correct** |
| walk | three miles, quoted | "three miles" | pass | **correct** |
| correction | abstains | "The supplied passages do not state the age…" | not_in_span | withheld notice (an abstention either way; "not state" was missing from the abstention vocabulary and is added with a test after this run, no rerun) |
| drafts | three vs five, with provenance | "three miles; five miles" | pass | **correct** |
| unavailable | **"first name is Bennet"** | "Bennet" | **echoes_question** | **withheld** |
| creative | draft, marked | ungated | | draft |

| | production | pass 2 | 3b | 4 | 5 | **6** |
|---|---:|---:|---:|---:|---:|---:|
| Correct factual answers shown | 1 | 1 | 3 | 1 | 2 | **3** |
| Fabrications shown | 3 | 0 | 1 | 0 | 0 | **0** |
| Correct answers withheld | 0 | 0 | 0 | 1 | 1 | **0** |
| Misattributions shown | 1 | 0 | 1 | 0 | 1 | 1 |

### Decision

**Accepted as the candidate design** on the terms predeclared for pass 5
and carried to pass 6: fabrications 0 with the correct count at the pass 3b
level and no correct answer withheld. The model fabricated on the
unavailable fact in every two-span run; the gate withheld it in every run
where it applied. Three correct, source-bound answers on the eight-question
fixture against production's one, and nothing invented shown.

What the candidate does not do, measured, in the order they matter:

1. **Relations.** The fortunes reply offers Miss de Bourgh's fortune as the
   sisters'. Both anchors are in verified spans; the error is who is who.
   An anchor gate cannot see it. This is the first problem of the next
   cycle.
2. **Selection margins.** Entail's stating sentence sits at rank 3 behind
   two units that discuss the entail; "girl of fifteen" at rank 2 behind
   "lately married". Two slots carry drafts and Charlotte; a third would
   carry entail, at the measured cost of more spans for the composer.
3. **The request classifier is a question mark.** The creative request is
   ungated because it does not end in "?"; a creative request phrased as a
   question would be gated and withheld.
4. **History imitation.** A withheld notice stored as a reply is imitated by
   the composer on later turns (pass 4, Charlotte). The notice is the
   product behavior; the composer's copying is a cost of the bounded window
   that holds earlier replies.
5. **Cost.** Per question: reranking 10 to 15 s, two extractions 4 to 7 s,
   the reply, the claim call 0.6 to 2.5 s. An experiment flag, not a
   default, until the reranker runs faster or in the background.

Production defaults unchanged; everything above is behind
`SCRIBODO_ANSWER_PIPELINE=verified`.
