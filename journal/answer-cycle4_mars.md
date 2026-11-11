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

Pending. The run needs the app to open a window; the console was locked when
it was launched (twice, exit 3 within two seconds, "could not create a
window"). A waiter launches it when the console unlocks; the results are
appended here when it has run.
