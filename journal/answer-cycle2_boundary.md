# Answer cycle 2 — fresh boundary questions through the real app

**Date:** 2026-11-09. **Protocol:** manifold synthesis, next executable cycle,
step 2, first half: measure *current* behavior on new source-bound questions
written before any model run (`tests/fixtures/answer_boundary/questions.json`,
eight cases across the six required kinds). No candidate contract was tested
yet; this run is the control it will be compared against. No production prompt,
selection policy or model changed.

**Run:** the frozen live-capture app from `build/answer-live-latest/scribodo`
(production request builders unchanged, private capture only), real model,
seed 42, requested 16K context, the same real-novel project and chat fixture as
the seven-question baseline, driven by `tests/ui/store/21_boundary_questions.scribo`
(generated from the fixture; two author notes are typed before the correction
and contradictory-draft questions). Artifacts: `build/product-memory-use/boundary-1/`
(messages, 821 source units, work copy) and `build/answer-boundary-capture/run/`
(10 complete request/retrieval/stream/content/outcome sets, all `capture_complete`).
Review sheet: `build/answer-boundary-capture/review.json`
(`tools/answer_boundary_review.py`). The evidence runner crashed after the app
run on a non-UTF-8 byte in the UI log (see "Harness defects"); the export was
performed afterwards with the same SQL and the disposable database was dropped.

A fake-backend dry run of the same script (`build/boundary-dry/`) validated the
script mechanics first: 20 passed, 7 source-delivery expectations failed because
the fake backend has no embedder, so only lexical retrieval ran.

## Source delivery (production retrieval, unchanged)

| Case | designated passage admitted | in final request |
|---|---|---|
| single_fact_entail | yes | yes |
| two_entities_fortunes (two passages) | yes, yes | yes, yes |
| single_fact_charlotte_age | yes | yes |
| single_fact_walk_distance | no | no |
| author_correction ("not sixteen") | no | no |
| contradictory_drafts ("only three miles") | no | no |
| unavailable_fact | n/a | n/a |
| creative_request (entail) | no | no |

Four of seven designated passages were admitted. "only three miles" was missed
twice; both those questions were then answered from a different journey
("twenty-four miles" to Gracechurch Street).

## Replies, mechanical columns and human review

Quotation pieces are classified by `tools/answer_citation_binding.py`
(normalized; markdown emphasis ignored). Review fields follow the fixture.

| Case | truncated | visible chars | quoted pieces: bound / other / preview / unsupported | fact selection | unsupported additions | citation binding | contradiction / correction | abstention | completion |
|---|---|---:|---|---|---|---|---|---|---|
| single_fact_entail | **yes (2,048)** | 8,604 | 18: 0/0/0/18 | fail: never states the entail | severe: rewrites the request as "write an essay", emits chat-fixture lines and an invented "PARROLAND ESTATE" heading, copies the attachment preview | fail | n/a | n/a | fail |
| two_entities_fortunes | **yes (2,048)** | 8,128 | 34: 0/0/5/29 | fail: neither value attributed | severe: pages of copied and paraphrased passages, "Signature Table" | fail | n/a | n/a | fail |
| single_fact_charlotte_age | no | 3,851 | 2: 1/0/0/1 | **pass**: twenty-seven, quoted from the right source | yes: appends the attachment preview and an invented claim about Mrs. Bennet refusing a visit | pass for the answer quote | n/a | n/a | poor: correct answer buried in 3.8K of extra text |
| single_fact_walk_distance | no | 628 | 1: 0/0/1/0 | fail: "twenty-four miles" from a different journey | yes | wrong source | n/a | **fail: should have abstained** (source absent) | yes |
| author_correction | no | 664 | 2: 1/0/0/1 | pass on supplied evidence: "girl of fifteen" from an admitted source (the designated "not sixteen" was not delivered; both are in the novel) | mild: "part09.md" artifact, unrequested Charlotte aside | pass | **pass**: names the earlier note as the error and supersedes it | n/a | yes |
| contradictory_drafts | no | 552 | 2: 0/0/2/0 | fail: current figure wrong (twenty-four miles) | no | wrong source | **pass in form**: keeps both figures with provenance, does not merge | fail: should have said the current figure was not in the supplied sources | yes |
| unavailable_fact | no | 391 | 1: 0/0/0/1 | **fail: "Mr. Bennet's first name is Bennet."** | yes: lists Charlotte as a daughter | fail | n/a | **fail** | yes |
| creative_request | no | 527 | 0 | n/a | canon drift inside the draft (estate "to Mr. Bingley's heirs", "marrying Mr. Collins") | n/a | n/a | n/a | **pass**: produces the draft, marked as new writing, no reclassification |

Tally on the contract's own terms: 2 passes (correction handling, creative
classification), 1 correct fact with poor completion, 5 failures. With the
designated source delivered (first three cases), 1 of 3 answered correctly.

## Two observations beyond the per-case review

1. **Packet and attachment markers leak into replies.** 9 of 10 replies in this
   run contain `--- END DOCUMENT ---`, `BEGIN DOCUMENT` or the "(too long: cut
   off)" preview text; the seven-question baseline had 1 of 7 and the 76
   diagnostic responses 8. The very first generation (entail) already misread
   the packet, labelled the attachment preview as the "Author's current request"
   and copied it; every later reply in the session then ends with a copied
   `--- END DOCUMENT ---`. Because assistant replies return as history, one
   degenerate reply shapes the rest of the session. This is an observation of
   current behavior, not a measured cause; it names a candidate experiment
   (attachment-preview placement and marker vocabulary; history hygiene for
   assistant text) that must be measured old-versus-new before anything ships.
2. **Quotation binding across all 43 earlier diagnostic replies, re-measured
   with emphasis-tolerant normalization and case-insensitive labels
   (REDTEAM-019):** 220 pieces, 46 bound to the cited source, 39 in another
   source, 34 only in the attachment preview, 101 in no supplied text. The
   boundary run adds 60 pieces: 2 bound, 0 in another source, 8 only in the
   attachment preview, 50 in no supplied text.

## Decision

Per the protocol, this measures current behavior; no candidate contract was
applied, so nothing is accepted or rejected yet. The next half of step 2 is to
run the same eight cases with an explicit factual task condition and compare
field by field, with the warning already written down: do not add instructions
merely to make these cases pass, and reject on boundary-task failure. The
unavailable-fact and absent-source cases are the ones most likely to decide it.

## Harness defects found and fixed along the way

- `build/product-memory-use-run.py` read the UI log as strict UTF-8 and crashed
  after the app run; it now reads with replacement and is tracked as
  `tools/answer_product_run.py` so the run is reproducible from the repository.
- The UI script runner's expectation detail is cut at a fixed byte length
  (`show()` in `src/app/script.c`), which split a multibyte character and
  produced the invalid byte; see the fix recorded with this entry.
