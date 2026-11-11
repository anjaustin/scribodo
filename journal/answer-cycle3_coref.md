# Answer cycle 3 — coreference at ingestion (selection by relation)

**Date:** 2026-11-10. **Starting fact:** cycle 3
(`journal/answer-cycle3_relation.md`) closed with the relation error located
and unsolved: a question asks for one entity's attribute, the manuscript states
it with a pronoun subject six sentences after the name ("They were rather
handsome; ... had a fortune of twenty thousand pounds"), and other passages
name the entity without stating the attribute. Eight arms asked the production
reranker to see that relation at unit, window, part and part-with-context
granularity, through a named-entity filter, a co-occurrence gate, a per-part
claim and embedder admission. None moved the sisters' unit into a slot
without losing another case. The reranker's signal is who is named.

This arm names them. The indexed text of each document unit is rewritten
once, at ingestion granularity, so that third-person pronouns carry the name
of what they refer to; the original unit is untouched and remains the only
thing quoted. The reranker, embedder, prompts, clips and selection shape are
the pinned production ones. Selection only: no generation, no live run.

## Method

Tool `tools/answer_coref_arm.py`; model-free test
`tests/test_answer_coref_arm.py` (in `make test`); outputs `build/cr1`.
Environment `build/venv-coref` (Python 3.12, torch 2.15.0, fastcoref 2.1.6,
spaCy 3.8.18 with `en_core_web_sm` 3.8.0, transformers 4.57.6: fastcoref's
loader breaks on transformers 5, and its Longformer needs an explicit eager
attention kind, shimmed in the tool).

1. **Clusters.** A dedicated coreference model clusters the mentions of each
   document: fastcoref's FCoref (distilled, 0.6 s per 11,000-character chunk
   on the CPU) or LingMess (Longformer, 3.5 s per chunk). Documents are cut
   at unit boundaries into chunks of at most 11,000 characters with a
   2,500-character prefix of the preceding text, seen but never rewritten,
   so a pronoun early in a chunk can find its antecedent. LingMess skips any
   document over 4,096 encoder tokens; the chunks peak at 3,433. Both models
   see the same 70 chunks of the 403 document units of the pass 6 run
   (`build/product-memory-use/pipe-new6`, Pride and Prejudice in eight
   parts, 694,517 bytes).
2. **Representative.** Software, no model: a cluster's name is its most
   frequent short mention that carries a proper noun and no pronoun or comma
   (ties to the shortest, then the earliest); else the earliest short
   nominal mention; else none. A proper noun is a capitalized word the tagger
   calls NNP somewhere other than at a sentence start ("Vain indeed must
   be..." and "Affectation of candour..." are not names). A nominal
   representative gets the pronouns inside it named from their own clusters,
   one level: "his sisters" with "his" in Mr. Bingley's cluster is
   "Bingley’s sisters".
3. **Rewrite.** Only third-person pronoun mentions change (he, she, they, him,
   her, them, his, their, hers, theirs; it and its only to a named
   representative; reflexives and first and second person never, since the
   models cluster dialogue "I" and "you" with whoever is speaking).
   Possessives take the manuscript's apostrophe ("Bingley’s", "the
   sisters’"). Two densities: **all** such pronouns, or **subjects** only
   (clause subjects and their possessives, where the relation lives). The
   form measured replaces the pronoun; an annotate form that keeps the
   pronoun and appends the name in brackets is implemented and unmeasured.
4. **Selection.** The document units are re-emitted with the resolved text,
   ids and order unchanged, chat units untouched, as a derived product
   directory, and `tools/answer_admission_arm.py` runs unchanged over it:
   the pinned embedder over every unit and query, the content-word lexical
   stand-in, the pinned reranker over the union pools, today's shape (whole
   question, embedder 32 plus lexical 32, reranker, greedy top two) and the
   24 variants of cycle 3. The baseline is the same arm on the original
   units (`build/ad2`, same code by hash). Designated units are located by
   passage in both and must match; they do.

| Variant | Clusters | Mentions | Pronouns rewritten | Units over the 2,000-byte clip | Model time |
|---|---:|---:|---:|---:|---:|
| original | | | 0 | 0 of 403 | |
| FCoref, all | 3,040 | 27,295 | 8,436 | 195 | 29 s |
| FCoref, subjects | 3,040 | 27,295 | 4,475 | 107 | 30 s |
| LingMess, all | 2,917 | 26,658 | 8,236 | 185 | 248 s |
| LingMess, subjects | 2,917 | 26,658 | 4,371 | 102 | 246 s |

Every designated passage survives every rewrite byte for byte and stays
inside the production clip of both the embedder and the reranker. (A first
draft of the rule, before the sentence-start check and the exclusion of
comma-bearing representatives, pushed "only three miles" past the clip;
that draft is not measured here.)

## What the models did to the units that matter

The stating sentence of the sisters' fortune, FCoref (both densities):

> Bingley’s sisters were rather handsome; had been educated in one of the
> first private seminaries in town; had a fortune of twenty thousand pounds;

LingMess, both densities, merged Mr. Darcy into Mr. Bingley's cluster in that
chunk and wrote **"Mr. Darcy’s sisters were rather handsome; ... had a
fortune of twenty thousand pounds"**. The entail sentence under FCoref (all)
and LingMess (all): "which, unfortunately for Mr. Bennet’s daughters, was
entailed"; under the subjects density "his daughters" is an object and stays.

Both models make errors a reader would not: FCoref writes "if Jane should
die" correctly but, in the correction unit, resolves "she" (Lydia) to
**"Lady Catherine’s daughter"**; LingMess writes "Mrs. Bennet was very
little disposed to approve" for Elizabeth and "Mr. Darcy listened to Jane"
for Elizabeth. These are the models' published limits (scores on news text,
nothing on fiction) showing on Austen's dialogue. The rewritten text is an
index, never shown, rebuildable, and the quoted unit is the original; the
errors cost ranking, not record.

## Selection, today's shape, per designated unit

Rank is among the whole-question union pool (reranker rank / embedder rank
in that pool / lexical stand-in score); SLOT means one of the two slots.

| Case | Unit | Baseline | FCoref all | FCoref subjects | LingMess all | LingMess subjects |
|---|---:|---|---|---|---|---|
| entail | 8 | – 3 / 2 / 0.57 | **SLOT** 2 / 3 | – 3 / 5 | **SLOT** 2 / 2 | – 3 / 3 |
| fortunes, sisters | 5 | – 7 / 3 / 0.33 | **SLOT** 2 / 2 | **SLOT** 2 / 1 | – 7 / 4 | – 7 / 4 |
| fortunes, Bingley | 243 | SLOT 1 / 1 / 0.67 | SLOT 1 / 1 | SLOT 1 / 2 | SLOT 1 / 1 | SLOT 1 / 1 |
| Charlotte | 65 | SLOT 1 / 3 / 0.40 | SLOT 2 / 2 | SLOT 2 / 2 | SLOT 1 / 2 | SLOT 1 / 2 |
| walk | 125 | SLOT 1 / 25 / 0.62 | SLOT 1 / 8 | SLOT 1 / 11 | SLOT 1 / 7 | SLOT 1 / 9 |
| correction | 119 | – not in pool | **SLOT** 2 / 39 (see below) | **SLOT** 1 / 36 (see below) | – not in pool | – not in pool |
| drafts | 125 | SLOT 2 / 18 / 0.21 | SLOT 1 / 14 | SLOT 2 / 13 | SLOT 2 / 12 | SLOT 1 / 15 |
| creative | 8 | – 8 / 13 / 0.23 | – 24 / 10 | – 21 / 11 | – 20 / 6 | – 26 / 12 |

The sisters' unit under FCoref: reranker score 0.6529 to **0.9900**, rank 7
to 2, and the second slot goes from Miss de Bourgh's unit to the sisters'.
Both designated units of the fortunes question are in the slots for the
first time in any arm, with nothing lost.

**The correction gain is an accident and does not count.** The unit was
never in the baseline pool: its lexical stand-in score was 0.167 (two of
twelve question words, "youngest" and "Bennet") against a cut of 43 units
above it. FCoref's wrong resolution of "she" to "Lady Catherine’s daughter"
added the question word "daughter", 0.25, and nine units above it: in the
pool by luck, where the reranker then scored it 0.97 on its own merits
("my youngest is not sixteen", "Miss Bennet"). The reranker's judgement of
the unit is real; its admission was not, and the lexical feeder here is a
stand-in for production retrieval anyway.

**The creative case** asks for new writing; its designated unit (the entail
clause, for grounding) falls from rank 8 to 20–26 under every variant and
was outside the slots before. Recorded; not a loss of a shown answer.

Totals under today's shape, and the best variant of cycle 3 for comparison
(parts with union pools of 32, reranker, matching), designated units in
slots out of 8 / in the pool out of 8 / cases complete out of 7:

| Selection | Baseline | FCoref all | FCoref subjects | LingMess all | LingMess subjects |
|---|---|---|---|---|---|
| today: whole, union 32, reranker, greedy | 4 / 7 / 3 | **7 / 8 / 6** | 6 / 8 / 5 | 5 / 7 / 4 | 4 / 7 / 3 |
| the same, correction accident removed | 4 / 7 / 3 | **6 / 7 / 5** | 5 / 7 / 4 | 5 / 7 / 4 | 4 / 7 / 3 |
| parts, union 32, reranker, matching | 5 / 7 / 4 | 7 / 8 / 6 | 7 / 8 / 6 | 5 / 7 / 4 | 5 / 7 / 4 |

All 24 selection variants improve or hold under FCoref (all); none regresses.
The full table is in `build/cr1/compare.json` and `build/cr1-run.log`.

## What this establishes

1. **The relation error is a selection error that software can reach.** The
   production reranker put the sisters' unit second at 0.99 the moment the
   unit said whose sisters they were. Nothing about the reranker, the
   embedder, the prompts, the clip or the slot rule changed. The eight arms
   of cycle 3 were asking a topical classifier a relational question; this
   arm gives it topical text that carries the relation.
2. **The coreference model decides the case.** FCoref linked "this man" and
   "They" to Bingley and his sisters; LingMess linked them to Darcy. The
   larger, better-published model was wrong on this chunk and the small one
   right. Neither is reliable on Austen's dialogue, and a wrong link
   produced the one false gain. Any adoption needs the resolution measured
   as its own metric on the fixture (named clusters that are right), not
   inferred from selection.
3. **Density matters less than correctness.** Subjects-only rewriting kept
   the fortunes gain, lost the entail gain (an object pronoun), and halved
   the units over the clip. Neither density pushed a designated passage past
   the clip, but 102 to 195 of 403 units now exceed it, so some stating
   passages elsewhere will be cut; a production form would be measured on
   the reach fixtures, not only these eight.
4. **Cost is ingestion-time and small.** FCoref over the whole manuscript on
   the CPU: 29 s. The chat model does not need to be loaded.

## Decisions that remain

1. Measure the live path: ingest the fixture through the app with resolved
   text as the indexed form (the original as the stored and quoted form),
   `SCRIBODO_ANSWER_PIPELINE=verified`, eight cases, against pass 6. This is
   a production change to indexing (an index layer beside the vectors, which
   are already a model-authored rebuildable index), not to prompts, and it
   needs a design note before it is built: where the resolved text lives,
   how it is rebuilt, and that it is never shown.
2. Measure resolution quality on its own: a small labelled set of pronoun
   links on the fixture (the sisters', the entail, the Lydia "she", the
   Elizabeth and Jane chains), FCoref against LingMess against the chat
   model, so the model that enters production is chosen on the fiction it
   will read.
3. Rerun the reach harness (`MEMORY-017`) on the resolved index at 30K and
   100K to see what the clip costs elsewhere before any density is chosen.

No production component changed. The derived products, admission runs,
stats, manifest with model versions and hashes, and the comparison are in
`build/cr1`.

## The live path, FCoref (`pipe-coref0` control, `pipe-coref1` treatment)

**Date:** 2026-11-10, after the offline arm above.

### The index layer

A production change behind a flag, default off, so the app can search and
rerank resolved text while storing, verifying, extracting and quoting only
the original. `src/app/memory_index_text.{h,c}`; migration 015; tests
`tests/test_memory_index_text.c` (in `make test`) and the schema-version
assertions in `test_db_core`. `make test`, `make dbtest`, `make analyze` (0 warnings
across 86 files), `make sanitize` and `make tsan` (8,343 checks) clean.

- `memory_units.index_text`, a nullable utf8mb4_unicode_ci TEXT with its own
  FULLTEXT key. NULL for every unit unless the experiment is on.
- `SCRIBODO_INDEX_TEXT_FILE` names a JSON object of {sha256 of the original
  unit text: resolved text}, strict UTF-8, at most 8,000 bytes each. A set
  variable whose file does not load aborts the app: an experiment that ran
  unresolved would be a wrong measurement, not a degraded one. The coref
  arm now writes this file beside its derived units (`index_text.json`).
- Ingestion (chat and document units) stores index_text: the file's
  resolution for that unit's hash, else a copy of the text. The lexical
  queries (chat and document) match `u.index_text` instead of `u.text`,
  with every source-binding check still on the original. The vector index
  embeds index_text (validated like the text; the 2,000-byte embedder clip
  applies as always). Hydration carries it in `MemoryUnit.index_text`. The
  verified pipeline reranks it; extraction and byte verification read the
  original; the composer sees only verified spans of the original. The
  recall view is untouched. Trace: `ANSWER_INDEX_TEXT enabled=1 units=N
  hydrated=M`.

### Runs

The same instrumented binary built from this tree
(`build/answer-live-coref`, source hashes in its manifest), the boundary
script, `SCRIBODO_ANSWER_PIPELINE=verified`, seed 42, real models: the
control with the flag unset, then the treatment with the FCoref all-pronoun
sidecar (`build/cr2/product-fcoref_replace_all/index_text.json`, 403 units,
sha256 `671c0b3d…`). Every hydrated unit in the treatment carried an index
text (trace, all eight questions); the control wrote no such line. Wall
time 387 s against 445 s; reranking 7.8 to 12.2 s per question against
10.1 to 14.6 s (longer units at the clip). Sheets
`build/answer-pipe-capture/review-pipeline-coref{0,1}.json`.

### Selection, measured with production retrieval

Designated unit's rank among the scored candidates, and its slot:

| Case | Control | Treatment |
|---|---|---|
| entail | 3, no slot | **2, slot 2** |
| fortunes, sisters | 7 at 0.65, no slot; Miss de Bourgh in slot 2 | **2 at 0.99, slot 2**; Miss de Bourgh out |
| fortunes, Bingley | 1, slot 1 | 1, slot 1 |
| Charlotte | 1, slot 1 | 2, slot 2 |
| walk | 1, slot 1 | 1, slot 1 |
| correction | not a candidate | not a candidate |
| drafts | 2, slot 2 | 1, slot 1 |
| creative | 9, no slot | 23, no slot |

Designated units in slots: **4 of 8 to 6 of 8**, the offline prediction
exactly, minus the correction accident, which did not recur: production
lexical retrieval is not the arm's content-word stand-in, and the wrongly
resolved "daughter" admitted nothing. Reach moved with real Qdrant and real
MariaDB full-text, no stand-in anywhere.

### What was shown

| Case | Control shown | Treatment shown |
|---|---|---|
| entail | abstention | **"unfortunately for his sons"**: the composer rewrote the quoted span and asserted the swap; gate `pass_no_anchors` |
| fortunes | hundred thousand right; Miss de Bourgh as the sisters' | **withheld**: the composer's claim was right for the first time in any run ("his sisters had a fortune of twenty thousand pounds") and wrote the inheritance as "£100,000"; the anchor gate cannot match digits against "a hundred thousand" (its documented scale-word limit) and withheld the reply |
| Charlotte | **27, correct** | abstention: the same two spans, order swapped; "neither refers to Charlotte Lucas" |
| walk | **three miles, correct** | abstention: slot 1 identical ("only three miles"); slot 2 changed from "walked three miles" (not in the treatment's candidate pool) to a lease sentence at 0.79; "does not explicitly say it is the distance to Netherfield" |
| correction | abstention | abstention; surfaced "girl of fifteen" |
| drafts | **three vs five, correct** | **three vs five, correct** |
| unavailable | withheld (gate) | abstained (composer); nothing invented |
| creative | draft | draft, marked |

| | control | treatment |
|---|---:|---:|
| Correct factual answers shown | 3 | 1 |
| Wrong statements shown | 1 | 1 |
| Correct answers withheld by the gate | 0 | 1 |
| Abstentions with the designated unit in a slot | 0 | 2 |
| Fabrications shown | 0 | 0 |

### What this establishes

1. **Selection by relation is solved on the live path for this case.** The
   sisters' unit is in the slot with production retrieval, and nothing in
   the reranker, embedder, prompts or clip changed. The composer then wrote
   the right attribution. Reach, the first metric of MEMORY-015, moved from
   4 to 6 of 8.
2. **Faithful use did not move, and the shown count fell.** Every loss is
   downstream of selection: a gate limit (digits against scale words), a
   noun swapped inside a quotation that the anchor gate has no anchor for,
   and two abstentions on correct evidence of the kind pass 7 recorded as
   composer variance under a changed window. The two metrics were kept
   apart for exactly this reading: the host's half is now right where it
   was wrong; the model's half is where it was.
3. **One run per arm cannot separate composition variance from the index
   effect.** The selection effect is deterministic (temperature 0 over
   fixed text) and is what this pass measured. The Charlotte and walk
   abstentions need repeated runs before they are attributed to anything.
4. **Pool composition changed, not only order.** The walk case lost its
   supporting second unit because lexical and semantic retrieval ran on
   resolved text and admitted a different set. Reach on the fixtures of
   MEMORY-017 should be rerun on the index layer before any density is
   chosen; this run says the change is not neutral.

### Decisions that remain

1. The anchor gate's number forms: currency and separators ("£100,000")
   against scale words ("a hundred thousand pounds"). A software change
   with a fixture case, measured against the recorded pre-gate replies as
   every gate change has been.
2. Quotation integrity: the composer's re-quotation of a verified span is
   not checked, and here it changed a word. A software check that every
   quoted string in a reply is a substring of a verified span is the
   candidate; measured against the recorded replies of passes 4 to 7 and
   these two runs before it ships.
3. Repeat the treatment (and the control) enough times to bound composer
   variance on Charlotte and walk; about 7.5 minutes a run.
4. The index layer stays behind `SCRIBODO_INDEX_TEXT_FILE`, default off. The
   sidecar is an experiment input: the app cannot produce it, and whether an
   ingestion-time resolver (today Python and torch) becomes an app
   dependency is a product decision, not a measurement.

## The gate's number forms, and the treatment rerun (`pipe-coref2`)

**Date:** 2026-11-10, after the live pass above. Decision 1 of that pass.

### The change

`answer_gate` in `src/llm/answer_pipeline.c`; tests in `test_answer_pipeline`
(162 to 177 checks). `make test`, `make analyze`, `make sanitize` and
`make tsan` clean.

- The claim tokenizer read "£100,000" as the anchors "100" and "000": a
  comma ends an alphanumeric run. A digit run now absorbs thousands
  separators (a comma followed by exactly three digits), so the anchor is
  100000. The currency sign was never an anchor.
- A digit anchor over 99 had no word form, so no span in words could
  satisfy it. A numeric anchor is now also matched by value against the
  number phrases of the normalized span that contain a scale word: "a
  hundred thousand pounds" is 100000, "two thousand a year" 2000,
  "twenty-seven thousand" 27000, "a hundred and twenty-seven" 127; a digit
  group with separators in the span ("1,500") is a value too. A bare small
  number or number word is not a value here, so "100" against "a hundred
  thousand" stays a mismatch and "20" against "twenty thousand" keeps the
  leniency measured in cycle 2. Pinned: the live claim itself, "100,000",
  "100000 pounds", "20,000" pass against the two fortunes spans; "100",
  "200,000", "1,000" do not; "100,00" is not a separator.

### Measured on the recorded claims before it shipped

`tools/answer_gate_replay.py` with `build/answer_gate_replay`, the gate's
own C code over the app's objects, replays every claim the live runs
recorded: 37 gated claims across passes 4 to 7 and the two coreference
runs, each with its question, its two verified spans and the fixture's
author turns. Against the committed gate's binary, built from the same
tree with only this file at HEAD, **this change flips exactly one
verdict**: the coref1 fortunes claim, `not_in_span` to `pass`. Against
the verdicts recorded at the time, 8 of 37 differ, all 7 others being
passes 4 to 6 recorded before the abstention vocabulary, numeral
canonicalization and compound-number fixes that have since shipped
(`build/gate-replay-numbers.json`, `build/gate-replay-head.json`).

### The rerun

Same recipe as `pipe-coref1`: a capture binary from this tree
(`build/answer-live-coref2`), the boundary script, the verified pipeline,
the FCoref all-pronoun sidecar; the only source difference is the gate.
Selection was byte-identical to `pipe-coref1` in every case: the same
units, scores, slots and spans (the reranker runs at temperature 0 over
fixed text). Sheet `build/answer-pipe-capture/review-pipeline-coref2.json`.

| Case | coref1 shown | coref2 shown |
|---|---|---|
| entail | "his sons", gate pass_no_anchors | identical reply, byte for byte: the same window, the same swap |
| fortunes | withheld on "£100,000" | **shown and right**: "Mr. Bingley's own inheritance: nearly £100,000"; "His sisters: twenty thousand pounds". The first correct attribution of both values in any live run. One unsupported aside ("unfinished inheritance") and a hedge that "they" refers to the sisters "in standard Austen context" |
| Charlotte | abstention | abstention, now as an explicit claim; same two spans |
| walk | abstention | **"only three miles", correct** |
| correction | abstention | **"fifteen"**, from the verified "Lydia was a stout, well-grown girl of fifteen": source-bound and true of the manuscript, not the fixture's designated "not sixteen"; the earlier note is dropped, in an imitated apology ("You're right, I apologize for the mix-up") |
| drafts | correct | correct |
| unavailable | abstained | abstained; nothing invented |
| creative | draft, marked | draft, marked |

| | pass 6 | coref1 | coref2 |
|---|---:|---:|---:|
| Correct factual answers shown | 3 | 1 | **4** |
| Wrong statements shown | 1 | 1 | 1 |
| Correct answers withheld by the gate | 0 | 1 | 0 |
| Abstentions with the designated unit in a slot | 0 | 2 | 1 |
| Fabrications shown | 0 | 0 | 0 |

Wall time 574 s against 445 s, reranking 13 to 22 s per question against
10 to 15 s, with the same binary shape and the same units; not attributed
to the change (the gate runs in microseconds) and not explained.

### What this establishes

1. **The gate fix did what the replay said and nothing else.** Fortunes is
   shown and right; every other verdict on recorded claims is unchanged.
2. **The walk and correction gains are not the gate's.** Their claims were
   in-span in both runs; in coref1 the composer abstained and in coref2 it
   answered, on identical spans. The composer window holds the newest three
   pairs, and the fortunes reply, now an answer rather than a withheld
   notice, sits in the window of the three questions that follow it
   (Charlotte, walk, correction). So these moves are plausibly downstream
   of the fix through the window, and they are also the variance pass 7
   recorded; one run cannot say which. Repeats remain decision 3.
3. **The remaining wrong statement is the composer's, byte for byte
   reproducible.** Entail's "sons" for "daughters" came out identical in
   both runs: same window, same output. It is not noise; it is what this
   model does with that window, and the anchor gate has no anchor for it.
   Decision 2 (quotation integrity) is now the first open item.
4. **Four correct of seven factual cases is the pipeline's best shown
   count**, on the same fixture that production answered once and pass 6
   three times, with nothing invented and nothing correct withheld.

### Decisions that remain

1. Quotation integrity (decision 2 above): a software check that every
   quoted string in a reply is a substring of a verified span, measured
   against every recorded reply before it ships. The entail reply is the
   fixture case.
2. Repeated runs of control and treatment to bound composer variance.
3. The unsupported aside in the fortunes reply ("unfinished") and the
   imitated apology in the correction reply are faithful-use costs the
   anchor gate does not see; recorded, not yet measured as a class.
