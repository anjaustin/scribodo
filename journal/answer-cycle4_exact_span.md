# Answer cycle 4: the verified span is the manuscript's bytes

**Date:** 2026-11-12. **Tree:** `a98189a` plus the changes recorded below.
**Direction:** "start on the exact-span item" (the finding recorded in
`journal/answer-cycle4_relation.md`, "a sixth defect").

## The item

The verified block tells the composer "Verified passage from the manuscript,
quoted exactly", and the receipt stores the same text as the source the author
can open. The text was the extraction model's rendering of the passage, kept
when it matched the unit after normalization: case folded, Markdown emphasis
dropped, dashes and whitespace folded and, since `ca908bd`, quotation marks
dropped. So a span could be shown with the model's straight quotes for the
manuscript's curly ones, a capital where the manuscript has a lowercase
mid-sentence start, a spaced hyphen for an em dash, or without the quotation
marks the manuscript put around the sentence. "Exact" held for words, not
bytes.

## Measurement before the change

Every verified span in every recorded run's receipts, against its unit's text
from the same run's unit export (40 runs, 575 spans), through the product's
verifier as it stood: all 575 verify (537 recorded `verified`, 38 recorded
`verified_prefix` whose stored prefix verifies whole). Of the 575 shown spans,
**327 were the unit's bytes and 248 were not**: 232 differed in quotation
marks, 11 in case, 4 in dashes or spacing, 1 in emphasis markers. Examples: the
manuscript's `‘Miss Catherine! I can read yon, now.’` shown without its
quotation marks; `she answered, “she doesn’t bide here…` shown as `She
answered, "she doesn’t bide here…`; `“What is his name?”\n\n“Bingley.”` shown as
`What is his name? "Bingley."`; `_That_ is how I’m loved!` shown without the
emphasis (`build/builder/span-replay.txt`, the script in the evidence archive).

## The change

`answer_verify_span` now returns the unit's own bytes at the match. The
normalization gained a map from every normalized byte to the source bytes that
produced it (`normalize_map`, the one implementation behind
`answer_normalize`); the verifier finds the model's span in the normalized unit
as before, exact or as the longest sentence-boundary prefix, then reads the
match back through the map and widens it over the quotation marks and emphasis
markers the normalization dropped at either edge, so a quoted sentence keeps the
manuscript's quotation marks and an emphasized phrase its markers. The
statuses, the minimum length and the prefix rule are unchanged; what changed is
whose bytes come back. The verified block, the receipt and the gate's spans all
take the verifier's output, so all three now carry the manuscript's text; the
quotation and relation checks compare normalized forms and are unaffected.

`test_answer_pipeline` (`test_verify`): the emphasized model span comes back as
the unit's plain sentence; straight quotes and a spaced hyphen come back as the
manuscript's curly apostrophe and em dash; a sentence the model quoted without
its quotation marks comes back with them; a span inside the manuscript's own
`*…*` comes back with the markers. The replay binary
(`tools/answer_gate_replay.c`) takes `{"span", "unit"}` items and returns the
verifier's result, which is how the measurement above was taken; the same run
on the new verifier returns the unit bytes for all 575.

## Live

Two live runs on this tree's capture app (`build/answer-live-exact`), compared
question by question with the previous tree's runs of the same scripts in the
same environment (`pp-final-index` with the index layer on, `mars-builder`;
`build/exact/*-compare.txt`, `build/product-memory-use/pp-exact-index`,
`mars-exact`).

| | Spans whose bytes changed | How | Requests changed | Gate verdicts and withholds changed | Fixture |
|---|---|---|---|---|---|
| First novel, index on (521 s) | 11 of 20 | 9 quotation marks, 1 dashes or spacing, 1 case | 8 of 10 | **0 of 7** (one verdict label moved from `pass_no_anchors` to `pass`, withheld both times) | 25 passed, the same 2 failed |
| Second novel (318 s) | 4 of 20 | all quotation marks | 7 of 10 | **0 of 7** | 25 passed, the same 2 failed |

No span changed in its words; every changed span is the manuscript's quotation
marks, case or dash where the model's rendering had been shown. A request
changes wherever a span's bytes change (the verified block carries them) and
wherever an earlier reply changed (the history carries it), and the composer's
replies change with their requests; what the gate decided, what it withheld and
what the fixture found did not change on either novel.

## Gates

`make test` 95,956 C checks and 168 Python tests, 0 failures (6 checks more
than the previous tree: `test_verify`); `make dbtest` 18,000, 0; `make analyze`
0 warnings across 88 source files; `make sanitize` 95,956, 0, 0 diagnostics;
`make tsan` 48,585, 0, 0 diagnostics. Every exit status 0
(`build/exact/gates-*.log`; the tally sums every `N checks` line).

## Hosted verdicts

CI and Trusted acceptance both passed on `b752592` (runs 43261904802 and
43261904934).

## What remains

The span shown is now the manuscript's bytes wherever the verifier matches. The
match itself is still found in normalized space, so the tolerance decides what
is accepted, not what is shown; the categories above say what that tolerance
has let through so far. Measured the same day under eighteen mutations of 681
recorded spans: `journal/answer-cycle4_tolerance.md`.
