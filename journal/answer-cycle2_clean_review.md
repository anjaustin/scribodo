# Answer cycle 2 — clean-window review arm (predeclared)

**Date:** 2026-11-10. **Question:** can the same pinned LFM3-1.7B, given a fresh
context window holding one manuscript unit, the question and a finished reply,
judge that reply correctly? The thinking-mode arms (pt1) measured in-generation
deliberation in the same window and are not evidence about a separate review
step; nothing in the record had asked the model to review a completed reply in
either a clean or the original window. This arm fills that gap before any
review loop is designed.

**Inputs (frozen):** the eight boundary replies from the cycle 2 run
(`build/product-memory-use/boundary-1/messages.json`, visible text after the
product's cleanup, as recorded in `build/answer-boundary-capture/review.json`),
the 821 exported source units of that run, and the ten captured production
requests. The designated passages are re-bound to exactly one unit each at run
time (`tools/answer_boundary_bind.py`); the run refuses an ambiguous binding.

**Tool:** `tools/answer_clean_review.py` (tests: `tests/test_answer_clean_review.py`).
Pinned model and server binary by SHA, Metal BF16, `-fa off`, one 32K slot so the
same-window control fits, temperature 0, seed 42, thinking off, prompt cache off,
strict JSON schema on every call. Output `build/cr1/`.

## Arms

| Arm | Window | Calls | Scored against |
|---|---|---|---|
| `claim` | clean: system reviewer line, one unit, question, reply | one per (case, unit): 7 designated units (fortunes has two) plus the 8 delivered units of the unavailable-fact request = 16 | predeclared acceptable verdicts below |
| `claim2` | identical request, sent again | 16 | `claim`: byte-identical content or not |
| `span` | clean: one unit, one quoted piece (≥25 chars, as the binding classifier cuts them) | one per (piece, unit): 101, of which the byte oracle finds 1 verbatim | byte oracle: normalized piece is a substring of the normalized unit |
| `same` | original production prompt + the reply as an assistant turn + the review request | 8 | the same predeclared verdicts |

Reviewer schema: `reply_claim` (string), `source_states` (answers / contradicts /
silent), `reply_verdict` (supported / contradicted / unsupported),
`quotations_verbatim` (all / some / none / no_quotations). Span schema: one
boolean. The instructions are fixed in the tool and are diagnostic; nothing here
is proposed as production text.

## Predeclared acceptable verdicts (from the cycle 2 human review)

| Case | Unit reviewed | Acceptable `reply_verdict` | Reason |
|---|---|---|---|
| single_fact_entail | entail unit | unsupported | the reply never states the entail |
| two_entities_fortunes | each of the two units | unsupported | neither value attributed |
| single_fact_charlotte_age | twenty-seven unit | supported | correct, quoted from that unit |
| single_fact_walk_distance | three-miles unit | contradicted | reply says twenty-four |
| author_correction | "not sixteen" unit | supported or unsupported | reply says fifteen; consistent but not stated |
| contradictory_drafts | three-miles unit | contradicted | reply says twenty-four |
| unavailable_fact | each of 8 delivered units | unsupported | no unit gives the first name |
| creative_request | entail unit | unsupported or contradicted | a draft, not an answer; canon drift may be flagged |

## What would count

- `claim` accepted on at least 14 of 16 and `span` at or above 90 percent
  agreement with the byte oracle, with few false positives, would justify
  measuring a clean-window review as the second check behind the byte-match
  gate.
- `same` materially below `claim` would confirm the prediction that the review
  inherits the original window's confusion.
- `claim2` differing from `claim` would mean temperature-zero review is not
  reproducible here and any gate would need a different determinism basis.
- The span arm's false positives are the figure that matters for a gate: a
  reviewer that affirms invented quotations is worse than no reviewer.

Results follow below once the run completes; nothing above is edited after it.

## Results (`build/cr1/`, run 2026-11-10)

141 calls, all `stop`, all parsed against the schema, 375 s of model time on one
32K Metal slot. `claim` and `claim2` were byte-identical on 16 of 16: temperature-zero
review is reproducible here.

### Claim arm (clean window), 14 of 16 verdicts in the predeclared set

| Case | Unit | `reply_verdict` | `source_states` | `quotations_verbatim` | Accepted | Prompt tokens |
|---|---|---|---|---|---|---:|
| single_fact_entail | entail | unsupported | silent | none | yes | 2,650 |
| two_entities_fortunes | twenty thousand | unsupported | silent | none | yes | 2,644 |
| two_entities_fortunes | hundred thousand | unsupported | silent | none | yes | 2,685 |
| single_fact_charlotte_age | twenty-seven | **unsupported** | **silent** | all | **no** | 1,617 |
| single_fact_walk_distance | three miles | contradicted | contradicts | none | yes | 823 |
| author_correction | not sixteen | **contradicted** | contradicts | none | **no** | 816 |
| contradictory_drafts | three miles | contradicted | contradicts | none | yes | 818 |
| unavailable_fact | 8 delivered units | unsupported ×8 | silent ×8 | none | yes ×8 | 336–762 |
| creative_request | entail | unsupported | silent | none | yes | 725 |

The threshold (14 of 16) is met, but two measurements take the credit away:

- **A constant "unsupported" answer scores 13 of 16** on this predeclared set
  (constant "contradicted" 3, constant "supported" 2). The reviewer beats the
  constant by one case.
- **`source_states` is wrong in 4 of the 14 cases where the truth is known**,
  and the four are exactly the four cases where the passage answers the
  question (entail, both fortune units, Charlotte). The reviewer called every
  answering passage silent. It was right on all 8 silent units and on both
  contradiction cases. So the accepted "unsupported" verdicts on entail and
  the fortunes came from "silent", not from reading the reply against a
  passage it recognised as relevant; and the one case whose correct verdict
  was "supported" failed for the same reason, while the same call reported the
  reply's quotations as "all" verbatim although one of its two pieces is
  invented.
- `reply_claim` for the entail case attributes to the reply an answer the
  reply never gives ("entailed on a distant relation"; the reply does not
  contain "distant relation"). The reviewer completed the reply from the
  passage.
- The author-correction call judged "Lydia is fifteen" as contradicting
  "not sixteen". Predeclared as a rejection; it is a reading error on an
  inference the model should not have been asked to make, and it is the kind
  of case a byte check cannot settle either.

### Same-window control, 2 of 8

| Case | `reply_verdict` | `source_states` | `reply_claim` (start) | Accepted | Prompt tokens |
|---|---|---|---|---|---:|
| single_fact_entail | supported | answers | "I am at BEGIN to start work on the topic…" | no | 13,506 |
| two_entities_fortunes | contradicted | answers | "I am at BEGIN to start work on the topic…" | no | 15,621 |
| single_fact_charlotte_age | unsupported | silent | "Charlotte Lucas is twenty-seven… Netherfield Par…" | no | 14,682 |
| single_fact_walk_distance | unsupported | silent | "…only twenty-four miles." | no | 13,234 |
| author_correction | contradicted | contradicts | "Lydia is fifteen; Charlotte Lucas is twenty-seven." | no | 12,615 |
| contradictory_drafts | unsupported | silent | "…twenty-four miles; older draft said five." | no | 13,356 |
| unavailable_fact | unsupported | silent | "Mr. Bennet's first name is Bennet." | yes | 12,420 |
| creative_request | unsupported | silent | **"Mr. Bennet's first name is Bennet."** | yes | 8,283 |

In the original window the reviewer echoed scaffolding as the reply's claim
(two cases), reviewed a different turn than the one just given (the creative
request's review restates the previous question's answer), and called the
three-miles contradiction "silent" twice. The two accepted verdicts are the
constant answer. The prediction held: the review inherits the window's
confusion, and the clean window is a different and much better task.

### Span arm, 35 of 101 agree with the byte oracle

| | Count |
|---|---:|
| Pieces whose normalized text occurs in the shown unit (oracle true) | 1 |
| Reviewer true on the oracle-true piece | 1 of 1 |
| Pieces not in the shown unit (oracle false) | 100 |
| Reviewer affirmed anyway (false positives) | **66** |
| …of which verbatim elsewhere in the same document | 6 |
| …of which verbatim in some unit of the run | 16 |
| …of which in no unit of the manuscript at all | 50 |

Fifty affirmed quotations exist nowhere in the manuscript. By case the false
positives are fortunes 42, entail 16, author-correction 2, drafts 2,
unavailable 2, Charlotte 1, walk 1. The reviewer is not a usable quotation
verifier: it says yes to two in three invented quotations. The byte match
already answers this question exactly, in software, and must be the gate.

## Reading

1. **Clean window beats same window decisively** (14 vs 2 of 8 scaled, with the
   same-window reviewer echoing scaffolding and reviewing the wrong turn). Any
   review step, if there is one, runs in a fresh window with one unit.
2. **The clean-window reviewer still cannot read for relevance.** It missed all
   four answering passages and the one supported reply. It detected both
   numeric contradictions and all eight silences. Its usable signal on this
   data is "the passage states a different short fact", nothing wider.
3. **It cannot verify quotations.** 66 percent false positives on invented
   text. Quotation fidelity is a software check, settled.
4. **Determinism holds** at temperature zero with a schema (16 of 16 identical).

Decision: the review-loop design keeps the byte-match gate as the only
quotation check and does not promote a model review of composed replies. The
measured capability worth carrying forward is narrower than a review: a single
short fact in one unit against a single short claim. That is the per-unit
extraction shape ("which sentence answers; quote it or NONE"), which the next
arm should measure directly on the same eight cases rather than inferring it
from this one. Nothing in production changed.
