# Answer cycle 4: a second manuscript, held out

**Date:** 2026-11-11. **Tree:** `b7a6ee2` plus the fixtures and tool changes
recorded below. **Direction:** the author asked for a second manuscript "pulled
from the web, preferably something more sci-fi", and made every remaining gap
P0 ([PRODUCT-GAPS](../docs/PRODUCT-GAPS.md), working order item 2).

## Why this manuscript

[AUDIT-003](../docs/AUDIT-003-repository-audit.md) and
[LANDSCAPE](../docs/LANDSCAPE.md) row 20 said the same thing: every selection
fix so far is general in form and was chosen against one novel and eight
questions, all in its first part. *Pride and Prejudice* is also the friendliest
possible text for the stack: third-person narration (the coreference layer
rewrites third-person pronouns only), canonical English names with honorifics,
double-quoted dialogue, written-out numerals.

*A Princess of Mars* (Edgar Rice Burroughs, 1912; Project Gutenberg #62) breaks
the first two assumptions and keeps the others, so a change in result points at
them:

- first-person narration throughout ("I", "my", "we"), so the index layer has
  nothing to rename in the stating sentences;
- invented proper names (Tars Tarkas, Dejah Thoris, Lorquas Ptomel, Woola,
  Zodanga, Thark) that a news-trained tagger has never seen;
- facts spread across the whole book, so the fixture can designate passages in
  every part rather than one.

## The text and how it enters

Plain text from Project Gutenberg, SHA-256
`d35d09d2b9b7691822909636085c6a01903bae375e51ee10b45377021b122b66`, 399,822
bytes, imported by `tools/manuscript_import.py` unchanged: 28 chapters,
65,966 words, 364,666 bytes, eight parts of 36 to 50 KB
(`build/manuscripts/a-princess-of-mars/`, source and checksum beside the
import; archived per [EVIDENCE](../docs/EVIDENCE.md)). The tool drops the
foreword, which precedes the first chapter heading; the chapter titles ("ON THE
ARIZONA HILLS") stay as the first line of each chapter. Nothing else was
changed.

## The held-out rule

The questions were written on 2026-11-11 from a reading of the imported parts,
before any pipeline or reach run on this novel, and checked mechanically: every
designated passage and every required text occurs exactly once in its named
part and nowhere else in the book. They are never tuned against. If this
fixture is ever used to choose a change, a third novel becomes the held-out set
and this record says so.

- `tests/fixtures/answer_boundary/princess-of-mars.json`: eight cases in the
  same kinds as the first fixture (single fact ×3, two entities with different
  values, author correction, contradictory drafts, unavailable fact, creative).
  Two of the single facts have the first-person narrator as the sentence's
  subject where the question names "John Carter" or "the narrator". The two
  entities live in different parts.
- `tests/fixtures/memory_reach/a-princess-of-mars-tasks.json`: fifteen
  document-fact reach tasks, at least one in every part.
- `tests/ui/store/22_mars_boundary_questions.scribo`: generated from the fixture
  by the new `tools/answer_boundary_script.py`; the first novel's script is now
  generated the same way and `tests/test_answer_boundary_fixture.py` holds both
  scripts to their fixtures.

Tooling changes for a second manuscript, no behaviour change for the first:
`tools/answer_product_run.py` takes `SCRIBODO_MANUSCRIPT`; the bind, review
and gate-replay tools take `SCRIBODO_BOUNDARY_FIXTURE`.

## Reach (selection only, production planner)

`tools/memory_reach_eval.sh --words 30000 --per-category 3 --manuscript
build/manuscripts/a-princess-of-mars/parts --manuscript-tasks
tests/fixtures/memory_reach/a-princess-of-mars-tasks.json`, the same shape as
the first novel's run in [MEMORY-021](../docs/MEMORY-021-real-manuscript.md):
the 30K fixture chat with the eight parts attached, real tokenizer and
embedder, 16 lexical and 16 semantic candidates, 8 admitted excerpts. Run
`build/memory-reach/runs/scribodo_test_reach_63447_e6cf18fb4b99bf71`; all
harness gates passed.

| Novel | Document tasks | reach@candidates | reach@admitted | lexical hit | semantic hit | planning p95 |
|---|---:|---:|---:|---:|---:|---:|
| Pride and Prejudice (MEMORY-021, 2026-11-08) | 14 | 1.00 | 0.71 | 0.93 | 0.93 | ~117 ms |
| Pride and Prejudice, control at this tree (`scribodo_test_reach_63867_5ba63ffa393d9b0d`) | 14 | 1.00 | 0.71 | 0.93 | 0.93 | 144 ms |
| A Princess of Mars (`scribodo_test_reach_63447_e6cf18fb4b99bf71`) | 15 | 1.00 | **1.00** | 1.00 | 0.80 | 108 ms |

Every one of the fifteen held-out passages was admitted. Three were found by
the lexical channel alone (semantic miss); none depended on the semantic
channel alone. The chat-fixture categories were unchanged at 1.00 admitted in
both runs.

The control settles the first question: the first novel's four misses are
unchanged at this tree (all four discovered, three by both channels, none
admitted), so the difference between the novels is not a change in the
planner between measurements. What remains is the second reading, and it
stands as the finding:

- **The planner generalises to a second book.** Nothing in the selection path
  knew this novel, its invented names or its first-person narration, and it
  admitted every held-out passage under production budgets.
- **The difficulty is the question, not the book.** The first novel's misses
  are attribute statements with a pronoun subject several sentences from the
  name ("They were rather handsome; ... had a fortune of twenty thousand
  pounds"), competing at admission against passages that name the entity.
  The Mars tasks were written to spread across the book and most carry the
  entity's name in the stating sentence; invented names are rare tokens, so
  the lexical channel rewards them. A held-out fixture measures the stack on
  questions its author would ask; it does not match the first fixture's
  difficulty, and this record does not claim it does. The next held-out set
  should include pronoun-subject attribute questions by construction, so the
  class the first novel exposed is measured on a second book.

## The verified pipeline, live (`mars-pipe0`)

Same recipe as the first novel's runs (`SCRIBODO_ANSWER_PIPELINE=verified`,
seed 42, the 30K fixture chat with the eight parts attached, real LFM3,
embedder and reranker), driven by `22_mars_boundary_questions.scribo` through
`tools/answer_product_run.py` with the live-capture build
`build/answer-live-mars`. Two launches failed in two seconds with "could not
create a window" while the console was locked; the run itself took 312 s once
the console was unlocked, with no change to anything else. 629 units; 25 of
27 script checks passed; the two failures are source-delivery checks,
discussed below. Review sheet `build/answer-pipe-capture/review-pipeline-mars0.json`.

### Selection

| Case | Designated unit rank (score) | In a slot | Note |
|---|---|---|---|
| sak leap (first-person subject) | 2 (0.981) | yes, slot 2 | top was a part07 unit at 0.989 |
| hordes (two parts) | 1 (0.998) and 2 (0.984) | both | |
| Kantos Kan | **7** (0.991) | no | top 0.997, second 0.997: six units naming Kantos Kan outscored the one that says what he is |
| Thark arrival (first-person subject) | 1 (0.999) | yes | |
| author correction | 4 (0.952) | no | slot 2 held part04's "I, the daughter of Mors Kajak, son of Tardos Mors", which states the same fact |
| contradictory drafts | 1 (0.946) | yes | |
| creative | 1 (0.969) | yes | |

Six of seven designated units reached a slot or had an equivalent statement
in one; the Kantos Kan unit is the one real selection miss, and it is the
first novel's sisters' failure in a new form: the question names the entity,
several passages name him, and the one that states the attribute ranks under
them with scores separated by 0.006. The index layer was not on in this run.
Both first-person-subject cases were selected and answered: the lexical and
semantic channels and the reranker do not need the subject named when the
question's content words are in the sentence.

### Replies

| Case | Reply | Gate | Outcome |
|---|---|---|---|
| sak leap | "I cleared a good hundred and fifty feet;" quoted, 150 feet | pass | **correct, shown** |
| hordes | both sentences quoted with their parts | pass | **correct, shown** |
| Kantos Kan | "The supplied manuscript passages do not contain that detail." | no_answer | abstained; the evidence was not delivered (selection) |
| Thark arrival | quoted; "Thirty days after his arrival on Barsoom" | pass | **correct, shown** |
| author correction | "She is the daughter of Mors Kajak (and son of Tardos Mors); i.e., she is NOT the daughter of Tardos Mors." | not_in_span | **correct, withheld** |
| contradictory drafts | "Current manuscript: twenty radium pumps; earlier draft: ten radium pumps." | not_in_span | **correct, withheld** |
| unavailable (horse's name) | states the passages do not give it; an aside about the burros, sourced | no_answer | correct abstention |
| creative | two sentences, marked as new writing | none | done |

Tally: 3 correct shown, 0 wrong shown, 2 correct withheld, 1 abstention for
missing evidence, 1 correct abstention, 0 fabrications. Against the first
novel's last treatment run (1 correct, 0 wrong, 3 wrong withheld, 2
abstentions on good evidence), the composer did better on the held-out book,
and nothing wrong reached the author on either.

### Why the gate withheld two correct answers

Replaying the two claims through the gate binary with their spans and author
turns, one token at a time (`build/mars/replay-*.json`):

- **"Current manuscript: twenty radium pumps; earlier draft: ten radium
  pumps."** Every number is sourced ("twenty" in the span, "ten" in the
  author's draft note, which the gate had: `author_turns=2`). The failing
  anchor is **"Current"**: a sentence-initial capitalised word, which the gate
  counted as a name anchor by design ("errs toward checking more"), absent
  from the spans and the author's earlier turns. The author's *question*
  contains "current manuscript"; the gate searched the question only for the
  echo check, never as a source for a sentence-initial word.
- **"She is the daughter of Mors Kajak (and son of Tardos Mors); i.e., she is
  NOT the daughter of Tardos Mors."** Every name is in span 2. The failing
  anchor is **"NOT"**: an all-capitals word written for emphasis, read as a
  name because it is capitalised.

Both are the same defect: capitalisation taken for a name. Neither appeared on
the first novel because its composer never wrote "Current ..." with a correct
figure or an emphatic capital in a correct claim. This is a software fix, and
it is chosen because of this run, so the record says so: **this fixture is no
longer held out for the gate.** It remains held out for selection, which the
change does not touch.

The change (`answer_gate` in `src/llm/answer_pipeline.c`): a sentence-initial
capitalised word that the question itself contains is a *soft* anchor, which
still counts toward the echo check but cannot fail the span check alone; an
all-capitals word of two or more letters whose lowercase form is a
closed-class English word ("not", "no", "never", "only", "all", "is", ...) is
emphasis, not an anchor. Names at the start of a sentence that the question
does not contain, and all-capitals words outside that list (acronyms), are
checked as before. Ten new checks in `test_answer_pipeline` (198 in all);
"Bennet" to "What is Mr. Bennet's first name?" still reads `echoes_question`.

Measured on every recorded claim, both novels (56 first-novel claims in
thirteen sheets, 7 Mars claims), old gate against new, same inputs:

| Claim | Old | New | Net effect on what the author sees |
|---|---|---|---|
| mars0 author correction | not_in_span | pass | a correct answer is shown |
| mars0 contradictory drafts | not_in_span | pass | a correct answer is shown |
| coref3 contradictory drafts ("Current manuscript: none; older draft: five miles") | not_in_span | pass | none: that reply was also caught by the quotation check at run time (`misquoted` recorded) and stays withheld |

No other verdict moved. The one first-novel flip is a wrong claim the old
gate had caught by accident, through the same false anchor; the quotation
check catches it on its own ground.

Gates on the change: `make test` 95,823 checks, 0 failures; `make analyze` 0
warnings across 86 files; `make sanitize` 0 diagnostics; `make tsan` 48,498
checks, 0 failures, 0 diagnostics (`build/mars/gates-*.log`).

## The verified pipeline with the gate change, live (`mars-pipe1`)

Same recipe, capture build `build/answer-live-mars1` (the gate change and
nothing else), 307 s, 634 units. Review sheet
`build/answer-pipe-capture/review-pipeline-mars1.json`.

**Selection is byte-identical to `mars-pipe0`**: every designated rank and
score the same, the same two source-delivery checks failing (Kantos Kan at
rank 7; the correction's designated part03 unit at rank 4 with part04's
equivalent statement in slot 2). The extracted claims are identical too, so
the gate change can be read on its own: the old gate replayed on this run's
claims withholds the same two, the new gate passes them, and nothing else
moves (`build/mars/replay-{before,after}-mars1.json`).

| Case | Reply | Outcome |
|---|---|---|
| sak leap | identical to pipe0 | **correct, shown** |
| hordes | identical | **correct, shown** |
| Kantos Kan | abstains | abstention, evidence not delivered |
| Thark arrival | identical | **correct, shown** |
| author correction | shown: quotes "I, the daughter of Mors Kajak, son of Tardos Mors", then writes "i.e., her father is Tardos Mors and her mother is Mors Kajak (who is presumably a title or rank)", then "So according to this passage, she is **not** the daughter of Tardos Mors; she is the **daughter of Mors Kajak**" | **shown with a wrong clause**: the conclusion is right and sourced; the intermediate sentence misreads the apposition and contradicts the conclusion. Counted as a wrong statement shown. |
| contradictory drafts | "Current manuscript: 20 radium pumps. Older draft: 10 radium pumps." with the span and the author's note each quoted | **correct, shown** |
| unavailable | states no verified passage names the horse; asks for the excerpt | correct abstention |
| creative | two sentences, marked as new writing | done |

Tally: 4 correct shown, 1 wrong statement shown (inside a reply whose final
answer is right), 0 withheld, 1 abstention for missing evidence, 1 correct
abstention, 0 fabrications.

Two things this run records that the first did not:

1. **What the false anchor had been hiding.** In `mars-pipe0` the correction
   reply was withheld for "NOT"; its claim was right. In this run the same
   claim passes and the author sees the reply, which carries a misreading of
   "daughter of X, son of Y" alongside the right conclusion. The old gate's
   catch was never of that misreading: the claim extractor had already
   reduced the reply to a correct sentence, and the gate reads the claim, not
   the reply. The anchor gate cannot see a wrong relation between two names
   that are both in the span. That is the "use" wall of MEMORY-012 and the
   relation question of cycle 3 again, now on the output side: faithful use
   is not a property the anchor gate measures, only names and numbers.
2. **The abstention sentence is copied forward within a run.** The correction
   reply opens with "The supplied manuscript passages do not give that
   detail. However, ..." although nothing was withheld in this run: the
   composer is echoing its own earlier abstention at Kantos Kan, three turns
   back in its window. Pass 4 called this history imitation of the *notice*;
   it is imitation of any earlier refusal in the window, including the
   composer's own. The reply still answers after the echo, so no answer was
   lost here, but the first sentence an author reads is false.

### Across the two novels at this tree

| | first novel, last run (`coref3`) | held-out novel, old gate (`mars-pipe0`) | held-out novel, new gate (`mars-pipe1`) |
|---|---:|---:|---:|
| Correct answers shown | 1 | 3 | 4 |
| Wrong statements shown | 0 | 0 | 1 |
| Correct answers withheld | 0 | 2 | 0 |
| Wrong replies withheld | 3 | 0 | 0 |
| Abstentions on missing evidence | 0 | 1 | 1 |
| Fabrications shown | 0 | 0 | 0 |

### Decisions that remain

1. **Window construction**, unchanged as the first item: the echo above is
   the same mechanism as the notice imitation, and the measurement is the
   same (replay a run's order with earlier refusals carried as a neutral
   marker or omitted).
2. **A relation check on the reply**, not the claim: the one wrong statement
   shown on either novel at this tree is a relation between two sourced names.
   Whether a deterministic check can hold "daughter of X" to the span without
   a model is an open question; the quotation check is the model for how such
   a check would sit beside the gate.
3. **The next held-out set** includes pronoun-subject attribute questions and
   relation questions by construction, so both failure classes are measured
   on a book the pipeline has not seen; this fixture now holds out selection
   only.
4. **The Kantos Kan unit** is the sisters' failure on a second book: six units
   naming the entity outscore the one that states the attribute, by 0.006.
   The index layer does not address it (the stating sentence already names
   him). It is a reranker saturation case and belongs with MODEL-002.

Evidence for this cycle is archived per [EVIDENCE](../docs/EVIDENCE.md)
(`scribodo-evidence-2026-11-11-mars.tar.gz`).
