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
