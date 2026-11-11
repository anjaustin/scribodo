# Answer cycle 3 — the relation error (selection granularity)

**Date:** 2026-11-10. **Starting fact:** pass 6 of the verified pipeline
(`journal/answer-cycle2_pipeline_live.md`, commit `e3d3324`) is the accepted
candidate design. Its one remaining error on the eight boundary cases is the
fortunes reply: Mr. Bingley's inheritance right, and Miss de Bourgh's fortune
offered as his sisters'. Both anchors sit in verified spans; the anchor gate
passed it. The journal named this the first problem of the next cycle.

## Where the error comes from (measured, pass 6 artifacts)

The question has two parts: the sisters' fortune (stated in part01 as "had a
fortune of twenty thousand pounds") and Bingley's own inheritance ("nearly a
hundred thousand pounds"). The pipeline reranked 55 candidates against the
whole question:

| Rank | Score | Unit |
|---:|---:|---|
| 1 | 0.9964 | Bingley inherited nearly a hundred thousand pounds (designated) |
| 2 | 0.9824 | Wickham on Lady Catherine: "Her daughter, Miss de Bourgh, will have a very large fortune" |
| 3 | 0.9530 | Chapter 1: "a single man in possession of a good fortune" |
| 4 | 0.8991 | Miss Bingley and the pair of fine eyes |
| 5 | 0.7518 | Longbourn one mile from Meryton |
| 6 | 0.7072 | "His sisters were fine women" |
| 7 | 0.6529 | Jane and Elizabeth on the sisters; "had a fortune of twenty thousand pounds" (designated) |

Two slots, so the de Bourgh unit was extracted, byte-verified and handed to
the composer as Source 2. The composer then wrote "his daughters' fortunes
include Miss de Bourgh". The error is upstream of composition: a unit that
answers no part of the question was admitted as evidence for one of them.

The cross-encoder arm (`journal/answer-cycle2_rerank.md`) had the same unit
at rank 7 among all 403 document units under the same reranker, so this is
the reranker's view of that unit, not a pool artifact.

**Why a relation gate is not the first move.** A gate that required a
claim's subject and value to co-occur in one verified span would have
withheld all three correct pass 6 answers: the walk span is "only three
miles" (no Elizabeth), the Charlotte span names only Elizabeth, the drafts'
second span is the bare figure. The spans the extractor copies are stating
clauses, and the subject usually sits a sentence or two before. Measured, not
assumed; recorded so it is not re-proposed.

## Arm 1: per-part reranking (`build/qp1`, `build/qp2`)

Tool `tools/answer_question_parts.py`; model-free test
`tests/test_answer_question_parts.py`. The question is split in software
(question sentences; a conjunction followed by a wh-word starts a new part;
an honorific's period is not a sentence end). Every candidate the app scored
in pass 6 is rescored through llama-server with the pinned reranker GGUF,
the C client's prompt and clip, against the whole question and each part.
`qp2` adds the context variant: the request's non-question sentences
prefixed to each part.

**Harness parity:** the offline whole-question scores reproduce the app's
logged scores to a maximum difference of 0.00005 over seven cases. The
offline path is the app's path.

Designated-unit rank among the case's candidates (lower is better):

| Case | Designated | whole | part 1 | part 2 | context part 1 | context part 2 |
|---|---|---:|---:|---:|---:|---:|
| entail | entail clause | 3 | 2 | **1** | | |
| fortunes | twenty thousand (sisters) | 7 | 5 | 20 | | |
| fortunes | hundred thousand (Bingley) | **1** | **1** | **1** | | |
| Charlotte | twenty-seven | **1** | **1** | | | |
| walk | three miles | **1** | **1** | | | |
| correction | not sixteen | not a candidate | | | | |
| drafts | three miles | 2 | 19 | 19 | **1** | **1** |
| creative | entail clause | 9 | 9 | | | |

Slots (two per question) holding a designated unit, over the seven cases:
today 4, one slot per bare part 4, one slot per context part **5**.

- Splitting helps when the part carries its referent (entail part 2 "why is
  that unfortunate for his daughters?" goes 3 to 1) and destroys the query
  when it does not ("Which figure does the current manuscript give?" scores
  under 0.02 everywhere; with the request's first sentence prefixed it is
  first). The context prefix is a required part of any split.
- The sisters' unit does not move into a slot under any form. For "What
  fortune do Mr. Bingley's sisters have?" the top five are 0.9858 (Bingley's
  inheritance), 0.9815 (Miss de Bourgh), 0.9814 ("His sisters were fine
  women"), 0.9734 (Longbourn), 0.9700 (the stating unit). The reranker
  saturates at unit level: the de Bourgh unit has "fortune", "poor Miss
  Bingley" and "his sister" within three sentences, and the stating unit's
  clause is sentence 11 of 12, with the sisters named only as "this man's
  sisters" and "they".

**A named-entity admission filter does not help** (model-free, on the saved
scores): requiring a unit to contain the question's capitalized names keeps
41 of 55 fortunes candidates ("Bingley" is in most of them, the de Bourgh
unit included) and leaves every designated rank where it was. Recorded so it
is not re-proposed.

## Arm 2: sentence windows of the top units (`build/wr1`)

Tool `tools/answer_window_rerank.py`; model-free test
`tests/test_answer_window_rerank.py`. For each query form, the top eight
units by unit score are cut into sentences and every window of three
consecutive sentences is scored against the query. Reports the best rank of
a window holding the designated passage among all windows, and the top
window.

Runs: `build/wr2` (width 3, 2,248 windows scored, 2 score failures counted
as 0 as the C client does) and `build/wr3` (width 6, 1,780 windows).

Best rank of a window holding the designated passage, among all windows of
the top eight units for that query (unit rank from arm 1 for comparison):

| Case, query | unit rank | window rank, width 3 | window rank, width 6 |
|---|---:|---:|---:|
| entail, whole | 3 | 3 | 4 |
| entail, part 1 | 2 | **1** | **1** |
| entail, part 2 | 1 | **1** | **1** |
| fortunes (sisters), whole | 7 | 19 | 19 |
| fortunes (sisters), part 1 | 5 | 18 | 11 |
| fortunes (Bingley), any | 1 | 1 | 1 (whole), 3 (part 1) |
| Charlotte | 1 | 1 | 1 |
| walk | 1 | 1 | 1 |
| drafts, whole | 2 | 2 | 5 |
| drafts, context part 1 | 1 | 9 | 5 |
| drafts, context part 2 | 1 | 6 | 13 |

For "What fortune do Mr. Bingley's sisters have?" at width 3 the windows
above the stating window (0.8367, rank 18) all score 0.99 or more:
Bingley's own inheritance, "Mr. Bingley's large fortune" twice (the
officers passage), Miss de Bourgh's fortune twice, "His sisters were fine
women". At width 6, where the stating window reaches back to "Miss Bingley
is to live with her brother", it scores 0.9884 at rank 11 under windows
such as "Mr. Bingley had danced with her twice, and she had been
distinguished by his sisters" (0.9978). The reranker's signal is the
entities and the word "fortune"; it does not carry the relation "the
sisters' fortune" at unit, window or part granularity.

Windows also cost the drafts case its first place (unit rank 1 to window
rank 5 to 9 under the context parts): the stating unit's sentences are
short and the figure sits in one of them without its referent. Coarse-to-
fine is not adopted.

**Model size is closed on the MVP machine.** This Mac has 16 GB; the app
holds the 1.7B chat model, the 0.6B embedder and the 0.6B reranker in BF16
(two 1.2 GB files for the small models). A 4B reranker is 8 GB in BF16.
Not measured; not possible to measure here.

## What the selection measurements establish

The sisters' fortune is not selectable with this reranker at any
granularity tried. The relation error will recur whenever a question asks
for one entity's attribute and the manuscript states it with a pronoun
subject while another passage states the same attribute of a different,
better-named entity. The remaining software route is the gate: the claim
call must report an answer per question part, so that software can see
that part 1 ("What fortune do Mr. Bingley's sisters have?") was answered
with no quantity at all ("a very large fortune", Miss de Bourgh's), while
part 2 was answered with one that is in its span. Whether the model can
report per-part answers is a prompt-shaped change and is measured against
the recorded pre-gate replies of passes 4 to 6 before anything ships (arm
3, below).

## Arm 3: per-part claims on recorded replies (`build/pcl1`)

Tool `tools/answer_part_claims.py`; test `tests/test_answer_part_claims.py`
pins the single arm's reader, task, schema and token cap to the C claim
call. The 21 gated pre-gate replies recorded by passes 4, 5 and 6 (seven
questions, three passes) are replayed through the pinned chat model with
two requests at temperature 0: `single`, the production claim call byte for
byte, and `parts`, the same reader with the question's parts listed and a
fixed-length answers array (one string per part, 128 tokens). All 42 calls
parsed; none truncated.

**One-part questions are unchanged** between the two requests: walk "three
miles" / "only three miles", Charlotte "27" / "twenty-seven", drafts "three
miles; five miles" / ["three miles", "five miles"], unavailable "Bennet" /
"Bennet", abstentions as abstentions. The schema change costs nothing there.

**The fortunes parts are never isolated.** For "What fortune do Mr.
Bingley's sisters have?" the per-part answer was:

| Pass | Part 1 answer (the sisters) | Part 2 answer (Bingley) |
|---|---|---|
| 4 | "nearly a hundred thousand pounds **(for his sisters)**; Mr. Bingley's own inheritance is said to be uncertain…" | the reply's two quotations, restated |
| 5 | "nearly a hundred thousand pounds (Mr. Bingley's inheritance); Her daughter, Miss de Bourgh, will have a 'very large fortune'…" | "nearly a hundred thousand pounds from his father; …" |
| 6 | same as pass 5 | same as pass 5 |

The model puts both values under part 1 and repeats Bingley's under part
2; in pass 4 the claim itself invents the attribution "for his sisters"
that the reply did not make. Because Bingley's number lands in part 1
every time, a rule "a quantity part answered without a quantity is not
answered" never fires on the misattribution (the four parts it flags are
abstentions, already handled by the abstention vocabulary), and the names
rule fires on "Bourgh" in two passes of three and would also fire on any
correct answer that cites a speaker. Two of the part answers leak a list
prefix ("1. The supplied text…"). The claim reader cannot attribute a
reply's values to the question's parts; the gate cannot be made per-part
on this model.

## Arm 4: the embedder can tell the difference (`build/qe1`, `build/ad1`, `build/ad2`)

Asked whether Qdrant's view distinguishes the two fortunes, a scratch
measurement embedded the 403 document units and the question's parts with
the production embedder and instruction (`build/qe1/report.json`):

| Query | sisters' unit | Bingley's unit | Miss de Bourgh unit |
|---|---:|---:|---:|
| whole question | 3 | 1 | 42 |
| "What fortune do Mr. Bingley's sisters have?" | **1** | 2 | 54 |
| "what is Mr. Bingley's own inheritance said to be?" | 3 | **1** | 64 |

The reranker lifted Miss de Bourgh's unit from 42nd to 2nd; the embedder
never had it near the top. The closed routes above are closed for the
cross-encoder, not for retrieval.

**The admission arm** (`tools/answer_admission_arm.py`, test
`tests/test_answer_admission_arm.py`) then gave each tool a job: parts
with their context sentence; a pool per part admitted by the embedder
alone or the embedder plus lexical overlap (content-word share, a stand-in
for production lexical retrieval) at sizes 8, 16 and 32; ordering by the
reranker, by the embedder, or by reciprocal rank fusion of both (the
conventional constant 60, not tuned); and the two slots assigned as a
maximum-score matching over parts and distinct units. Scored as designated
units in slots over the seven cases with designated units (8 units).
Today's shape (whole question, embedder 32 plus lexical 32, reranker,
greedy top two) reproduces the pass 6 selection exactly, Bingley plus Miss
de Bourgh. Deterministic: `ad1` and `ad2` agree on every shared variant.

| Variant (pool 32) | slots with a designated unit | of 8 |
|---|---:|---:|
| today: whole, embedder+lexical, reranker, greedy | 4 | |
| parts, embedder only, reranker, matching | 4 | fortunes 1 |
| parts, embedder+lexical, reranker, matching | **5** | entail gained; fortunes 1 |
| parts, embedder only or +lexical, embedder order, matching | 2 | fortunes 2, nothing else |
| parts, embedder only or +lexical, rank fusion, matching | 3 | fortunes 2; walk, drafts, entail lost |

Pools of 8 and 16 lose walk and drafts outright (their stating units sit
25th and 18th for the embedder) and never change fortunes.

The disagreement, from the persisted scores (`ad2/results.json`):

| Part | stating unit, embedder rank | reranker rank | reranker top five spread |
|---|---:|---:|---|
| sisters' fortune | **1** | 5 | 0.9858 to 0.9700; "Mr. Bingley was good-looking… His sisters were fine women" at 0.9814 |
| walk | 25 | **1** | 0.9927 then 0.9141 |
| drafts, current figure | 18 | **1** | 0.9668 then 0.9621 |
| entail, why unfortunate | 34 | **1** | 0.9877 then 0.9608 |

The two rankers are each right where the other is wrong, and no admission
size separates the cases: the walk unit needs a pool of at least 25 and
"His sisters were fine women" enters the sisters' pool at 8. Fusion trades
three cases for one. A margin rule ("defer to the embedder when the
reranker's top is not decisive") would fit these seven cases and is not
proposed on seven cases.

**What the arm does establish:** context parts plus matching, with the
reranker ordering a pool of 32, is strictly better than today's selection
(5 of 8 against 4, every other case unchanged) and costs nothing the
measurements can see except one more reranking per extra part. That is
the one change this cycle recommends to the candidate design; the sisters'
fortune is not fixed by it.

## Conclusion of the cycle

Every software route to the relation error was measured. Selection cannot
be made right for the sisters' part without making three other cases wrong,
with the pinned models on the MVP machine:

| Route | Measured | Result |
|---|---|---|
| Rerank per question part | arm 1, `qp1` | sisters' unit 7 to 5; never a slot |
| Context-prefixed parts | arm 1, `qp2`; arm 4 | fixes drafts (19 to 1) and entail (3 to 1); sisters unchanged; **recommended with matching** |
| Named-entity admission filter | arm 1, model-free | "Bingley" in 41 of 55 candidates; ranks unchanged |
| Sentence windows, width 3 and 6 | arm 2, `wr2`, `wr3` | sisters' window 18 and 11; drafts loses first place |
| Larger reranker | 16 GB machine | 8 GB in BF16 beside the chat model; not possible here |
| Subject-and-value co-occurrence gate | pass 6 spans | withholds all three correct answers |
| Per-part claim gate | arm 3, `pcl1` | the model never isolates the sisters' part; invents the attribution once |
| Embedder admission, embedder order | arm 4, `ad2` | fortunes right; walk, drafts, Charlotte lost |
| Embedder admission, reranker order, matching | arm 4, `ad2` | 5 of 8; fortunes unchanged |
| Rank fusion | arm 4, `ad2` | fortunes right; walk, drafts, entail lost |

The error class is: a question asks for one entity's attribute, the
manuscript states it with a pronoun subject six sentences from the name,
and other passages name the entity without stating the attribute. The
embedder sees whose attribute it is; the cross-encoder sees who is named.
Pass 2 (one span) answered this case correctly by saying the passage does
not mention the sisters, and lost two other correct answers for it (journal
cycle 2); that trade is already measured.

What the candidate design shows for this case: two byte-verified
quotations with their sources named, so the misattributed value appears
under "From Source 2 (part02.md)" with Miss de Bourgh in the quotation. The
relation is the composer's, and nothing in the pipeline can check a
relation.

Decisions that remain:

1. Adopt context parts with matching in the pipeline (5 of 8 against 4),
   measured live before it ships; keep this error class recorded as a
   known limit with the fixture case as a permanent regression probe.
2. For a multi-part factual question, show the verified quotations only
   and withhold the composed prose: nothing invented can be shown, at the
   cost of the drafts answer, which was correct and composed.
3. A different model in the reranker, claim or extraction roles (the
   remote adapter exists; the P0 is local).

Tooling of this cycle (`tools/answer_question_parts.py`,
`tools/answer_window_rerank.py`, `tools/answer_part_claims.py`,
`tools/answer_admission_arm.py`, their tests, outputs `build/qp1`, `qp2`,
`wr2`, `wr3`, `pcl1`, `qe1`, `ad1`, `ad2`) is committed so the
measurements can be rerun; no production component changed.
