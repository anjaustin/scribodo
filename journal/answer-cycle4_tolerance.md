# Answer cycle 4: the verifier's tolerance measured

**Date:** 2026-11-12. **Tree:** `efd6941` plus the changes recorded below.
**Direction:** "Let's wrap up the loose ends. 100/100. Methodically." The
first loose end is the one `journal/answer-cycle4_exact_span.md` left: the
verified span shown is the manuscript's bytes, but the match is still found in
normalized space, so the normalization decides what is accepted. This journal
measures that tolerance.

## Method

Every verified span a recorded run showed, with its unit's text from the same
run's unit export (`build/product-memory-use/*/messages.json` and
`units.json`): 681 spans from 38 runs. Each pair goes through the product's
verifier (`build/answer_gate_replay`, `{"span","unit"}` items, the C code
linked from the app's objects) unchanged and under eighteen mutations of the
span, in three families (`tools/answer_span_tolerance.py`;
`test_answer_span_tolerance` pins that each mutation changes what it claims to
and that the meaning mutations land inside the first sentence, where the
prefix rule cannot rescue them):

- **orthographic**: upper case, straight quotes for curly, double quotes
  removed, a spaced hyphen for a dash, doubled spaces, emphasis markers
  removed. Rendering, not words. The verifier is meant to accept these.
- **meaning**: a negation added or removed, a name replaced by another name
  from the same unit, a word dropped, two adjacent words swapped, an
  apostrophe lost inside a word (`it’s` to `its`), a hyphen joined
  (`twenty-five` to `twentyfive`), a number changed. Words. The verifier must
  reject these.
- **edge**: a plural possessive apostrophe lost (`the officers’ wives` to `the
  officers wives`), a marker inside a word (`har_dly`), a hyphen split into a
  space (`twenty five`), a fabricated sentence appended, the last sentence of a
  multi-sentence span negated. The cases the rules cannot tell from rendering,
  or hand to the prefix rule.

A mutation is applied only where it applies, so the counts differ per row.

## Before the change (`build/tolerance/before.txt`)

| family | mutation | applied | verified | prefix | rejected |
|---|---|---|---|---|---|
| unchanged | unchanged | 681 | 681 | 0 | 0 |
| orthographic | upper case | 681 | 681 | 0 | 0 |
| orthographic | doubled spaces | 681 | 681 | 0 | 0 |
| orthographic | straight quotes | 172 | 172 | 0 | 0 |
| orthographic | double quotes removed | 127 | 127 | 0 | 0 |
| orthographic | emphasis markers removed | 23 | 23 | 0 | 0 |
| orthographic | spaced hyphen for a dash | 1 | 1 | 0 | 0 |
| meaning | adjacent words swapped | 590 | 0 | 0 | 590 |
| meaning | word dropped | 565 | 0 | 0 | 565 |
| meaning | negation added or removed | 436 | 0 | 0 | 436 |
| meaning | number changed | 282 | 0 | 0 | 282 |
| meaning | name replaced | 263 | 0 | 0 | 263 |
| meaning | hyphen joined | 96 | 0 | 0 | 96 |
| meaning | apostrophe lost inside a word | 28 | 0 | 0 | 28 |
| edge | fabricated sentence appended | 598 | 0 | 598 | 0 |
| edge | **marker inside a word** | 568 | **568** | 0 | 0 |
| edge | last sentence negated | 214 | 0 | 201 | 13 |
| edge | hyphen split | 96 | 96 | 0 | 0 |
| edge | plural possessive apostrophe lost | 12 | 12 | 0 | 0 |

Every one of the 2,260 meaning mutations was rejected; every one of the 1,685
orthographic mutations verified and returned the unit's bytes. Of the edge
cases, one was a defect of the tolerance: a `*` or `_` was dropped wherever it
stood, so `har_dly` verified as `hardly` (568 of 568). The other four are
either the intended tolerance or the prefix rule doing its job:

- A **hyphen split** (`twenty five` for `twenty-five`) is the same words; the
  manuscript's hyphen comes back in the shown bytes. Accepted by design.
- A **plural possessive apostrophe** after `s` and before a space cannot be
  told from a closing single quotation mark (the third novel's dialogue is in
  single quotes), so it is dropped like one; the words are unchanged and the
  manuscript's apostrophe comes back in the shown bytes. 12 of 681 spans carry
  the form. Accepted, recorded.
- A **fabricated sentence appended** to a correct span is cut by the prefix
  rule: the correct sentences are shown, with the status `verified_prefix`,
  and the fabricated one is not. 598 of 598.
- The **last sentence negated** is cut the same way (201); the 13 rejections
  are spans whose only sentence boundary before the mutation was `Mr.` or
  another prefix shorter than the minimum span, so nothing could be kept.

## The change

`emphasis_mark_at` (`src/llm/answer_pipeline.c`): a `*` or `_` is an emphasis
marker, and dropped, only at a word's edge; one between two letters or digits
is part of the word and stays, so a span carrying it does not verify. The same
function decides the edge widening (`dropped_mark_at`), so a manuscript's own
marker at the edge of a match still comes back with the span. The Python
mirror (`normalize` in `tools/answer_citation_binding.py`, used by the relation
arm and the unit extraction tool) follows the same rule. The three
manuscripts have no marker inside a word (the only match under
`build/manuscripts` is in a log file), so no recorded or possible span of theirs
changes status; the rule closes a path, not a case. `test_normalize` had
pinned the old fold (`was_entailed` to `wasentailed`) and now pins the new;
`test_verify` adds the rejected in-word marker, the still-folded edge marker and
the normalized forms; `test_answer_citation_binding` pins the mirror.

## After the change (`build/tolerance/after.txt`)

Identical to the table above in every row but one: **marker inside a word:
568 applied, 0 verified, 568 rejected.** The 681 unchanged spans return the
same bytes and statuses as before (`build/tolerance/unchanged-before.jsonl`
equals `unchanged-after.jsonl`). The gate replay over the recorded claims and
replies (`tools/answer_gate_replay.py`, 105 claims, 126 replies, 206 quotation
pieces) is byte-identical before and after (`build/tolerance/gate-before.txt`,
`gate-after.txt`): the normalization is shared by the span verifier, the claim
gate and the quotation check, and none of them saw a verdict move.

No live run was taken for this change: no recorded request can change (the
681 shown spans are byte-identical, and the request carries the span), and the
rule only affects a span with a marker inside a word, which no manuscript or
recorded reply contains. The measured tolerance after the change is the
tolerance the design intended: case, quotation marks, dashes, whitespace and
emphasis at word edges; the words themselves must be the manuscript's.

## Gates

`make test` 95,959 C checks and 173 Python tests, 0 failures (three C checks
and five Python tests more than the previous tree: `test_verify`, the tool's
test and the mirror's); `make analyze` 0 warnings across 87 source files;
`make sanitize` 95,959, 0, 0 diagnostics; `make tsan` 48,589, 0, 0
diagnostics; `make dbtest` 18,000, 0. Every exit status 0
(`build/tolerance/gates-*.log`, `gates-exit.txt`; the tally sums every
`N checks` line). The first `make test` on this tree failed one check:
`test_normalize` had pinned the old fold of `was_entailed`, the very behaviour
the measurement found; the pin was changed to the new rule and the gates were
rerun from the start. `make dbtest`, the last of the five, ran while the next
step's `rel_supported` hunk was already in the working tree, so `make test`
(95,959 and 173, 0) and `make dbtest` (18,000, 0) were run again on the
committed tree `5f9b53f` alone, with the working changes stashed
(`build/tolerance/commit-test.log`, `commit-dbtest.log`, `commit-exit.txt`),
before the push.
