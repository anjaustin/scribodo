# REDTEAM-020: adversarial review of the selection-measurement tooling

**Date:** 2026-11-10
**Reviewed tree:** `d4cdc7b..8ef558b` (five commits): the clean-window review
arm (`tools/answer_clean_review.py`), the per-unit extraction and ranked-span
arm (`tools/answer_unit_extraction.py`), the ranker instruction comparison
(`tools/answer_ranker_instruction.py`), the live-capture build variant
(`tools/answer_live_capture.py --query-instruction`), the live delivery
comparison (`tools/answer_live_delivery_compare.py`), their five test files,
the Makefile and TESTING.md wiring, and five cycle journals.

## Method

Every changed line was read against the question "what number did this
produce, and what would make that number wrong without anyone noticing?"
Suspicions were measured against the recorded runs (`build/cr1`, `ux1..ux4`,
`ri1`, `answer-ri-capture`) before being written down. Each correction has a
mutation control that reintroduces the defect into a generated copy of the
tree (`build/redteam020-mutations/`, tracked sources untouched) and must fail
the test the correction passes. Journal figures were recomputed from the saved
data. The full target matrix was rerun at the end.

## Findings and remediations

### R1 — the fabrication screen was defeated by Markdown emphasis (P2, fixed)

`check_reply` in `tools/answer_unit_extraction.py` tested the composed reply
with `first name (is|was) [A-Z]\w+` to catch a guessed first name. The run 4
all-spans reply for the unavailable fact was "Mr. Bennet's first name is
**Bennet**": the asterisks sit where the regex expects a capital letter, so
the screen passed it. Measured on the saved reply: raw text no match, text
with emphasis removed match. The case was still recorded as a failure, but
only because the separate abstention screen did not match; the screen built
for exactly this fabrication was blind to it.

Fix: `plain()` strips `*` and `_` before every must and must-not check, as the
citation classifier already does for quotations. Re-running every recorded
composition in `ux1..ux4` through the fixed check changes no pass/fail
verdict. Test: `test_checks_see_through_markdown_emphasis`. Mutation M1
(check the raw reply) killed.

### R2 — an unparseable extraction was recorded as the model saying "none" (P3, fixed)

One run 4 extraction hit its 256-token cap mid-span; the grammar-constrained
reply was cut JSON, `json.loads` failed, and `verify()` returned status `none`
and `answers: false`, the same record as a model that read the unit and
returned an empty span. Run 4's "answers 57 of 58" therefore counted a cap
artifact as a considered negative.

Fix: `verify()` returns `unparsed` for a reply that did not parse; the summary
counts it separately. `build/ux4/record-rescored.json` re-derives every status
from the saved responses: one call moves to `unparsed`, no other figure moves;
the journal carries the addendum. Test:
`test_unparseable_extraction_is_counted_as_unparsed_not_none`. Mutation M2
(drop the branch) killed.

### R3 — the reviewer's constant-answer baseline lived only in the session (P3, fixed)

The clean-window review journal reports that a constant "unsupported" scores
13 of 16 on the predeclared set and that the reviewer's 14 beats it by one.
That figure was computed ad hoc at the terminal; `score.json` did not contain
it, so the tool's own output over-stated the reviewer. Fix: `score()` writes
`constant_baselines` and `best_constant_claim_accepted`; `build/cr1/score.json`
re-scored from the saved responses reads 14 against 13. Test:
`test_constant_baselines_are_reported_beside_the_reviewer`. Mutation M3
(baseline hard-wired to zero) killed.

### R4 — predeclared counts were counted by hand and were wrong twice (P3, fixed)

The clean-review predeclaration said 15 claim arms (16; corrected before the
run after a dry build). The ranker-instruction predeclaration said 9 expected
spans (10; corrected in the results section). Both errors came from counting
cases instead of asking the plan. Fix: `--dry-run` on the clean-review,
extraction and ranker tools prints the arm and expected counts from the data
without any model; the dry runs on the real inputs read 16 / 16 / 101 / 8
arms with constant baselines 2 / 3 / 13, 58 extraction calls with 9 expected
units (5 realistic, 4 added), and 10 expected spans (5 and 5). Tests:
`test_plan_counts_match_the_plan`, `test_pool_counts_come_from_the_data`.
Mutation M5 (total omits the added pool) killed.

### R5 — the live-capture semantic variant: verified to touch one literal (P3, verified)

`instrument_semantic` replaces a three-line anchor in `src/llm/semantic.c`.
The test asserts the variant equals the original with the anchor removed,
byte for byte, so nothing else in the embedding client moved; the built
binaries were checked with `strings` (production prefix present in `old`,
absent in `none`). Mutation M4 (variant adds a declaration) killed. The
vector fingerprint includes the instruction, so the variant indexed its own
collection; `tools/services.sh testdb drop` deletes collections by database
prefix, so the variant's collections were removed with the test database.

### Reviewed without change

- **Arm file names contain colons** (`extract:case:0-request.json`). Legal on
  macOS, the MVP target; would need escaping on other filesystems. Noted.
- **Abstention screen false negative.** `ABSTAIN` missed one true abstention
  ("not a first name beyond that title"). The screen is a filter; the journals
  read every mechanical pass by hand and say so. Widening the regex on one
  observed phrasing would fit the sample, not the behavior. Left as is.
- **Admission check by substring.** The live compare tool tests a 64-hex unit
  id against the `memory_sources` string; a substring match on 64 hex
  characters cannot alias. Noted.
- **Replies differ across identical-delivery runs.** Unit, document and
  revision identifiers are per import and appear in packet headers, so
  seed 42 does not reproduce replies across imports. Expected; the
  measurement is delivery, not replies.
- **Journal figures recomputed from data:** clean review 66 false positives,
  14 accepted, 2 same-window; run 3 expected-false verified 34, expected-true
  8; run 4 expected-true 9, expected-false verified 20, rejected 27, 4 of 5
  realistic first places; `ri1` 6 first places under every instruction and
  top-5 units 3/3/3/5; live run semantic rank 10 for three miles and no
  candidate for the creative unit. All match the journals.
- **Lexical search observation (not a defect in the reviewed tree):** the
  three-miles unit contains "Netherfield" and "Elizabeth" yet never appeared
  among the lexical candidates (MariaDB natural-language FULLTEXT, score
  divided by length^0.75, 16 candidates). Recorded for the next arm, which
  measures lexical overlap directly.

## Final verification

Full target matrix on the remediated tree (`build/redteam020-results.json`,
logs `build/redteam020-<target>.log`), 17 targets, all exit 0, 736.3 s:

| Target | Result |
|---|---|
| `test`, `sanitize` | 95,593 C checks, 0 failures, 26 binaries; 96 Python tests in 17 files, all OK |
| `tsan` | 48,300 C checks, 0 failures; 0 data-race reports |
| `dbtest` | 18,000 C checks, 0 failures |
| `analyze` | 0 warnings across 83 source files |
| `uitest`, `uitest-db`, `uitest-real`, `offlinetest`, `backuptest`, `remotetest`, `cstest`, `llmtest`, `analyzetest`, `dependency-report`, `all` | exit 0 |
| `tools/test_audit_mutations.py` | 10 killed, 0 survived; both controls passed |

REDTEAM-020 mutation controls (`build/redteam020-mutations/results.json`):
M1 emphasis not stripped, M2 unparsed recorded as none, M3 constant baseline
hard-wired, M4 semantic variant touching more than the anchor, M5 pool total
omitting the added pool: 5 of 5 killed; the four unchanged test files pass as
controls. Tracked Markdown: 113 files, 204 relative links, 0 broken (one
tracked fixture, `tests/ui/fixtures/invalid.md`, is intentionally not UTF-8
and is skipped). `git diff --check` clean; `pyflakes` clean over the changed
tools and tests.

The cross-encoder arm's tool and test (`tools/answer_rerank_arm.py`,
`tests/test_answer_rerank_arm.py`) were added after the matrix ran; the
final `make test` on the committed tree is recorded in the commit message.
