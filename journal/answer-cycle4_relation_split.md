# Answer cycle 4: kinship and possession in the relation check, measured apart

**Date:** 2026-11-12. **Tree:** `5f9b53f` plus the changes recorded below.
**Direction:** "Let's wrap up the loose ends. 100/100. Methodically." The
second loose end: the relation check (`journal/answer-cycle4_relation.md`)
reads one closed word list that mixes kinship (father, daughter, nephew, …)
and possession (estate, fortune, inheritance, property). The relation journal
recorded that "the possession words keep the precision cost they had when
they were added" without measuring that cost on its own. This journal does.

## Method

Every captured live run with a composer reply (23 capture directories under
`build/*/capture-*`, the broken retire run excluded; 230 replies, 103 distinct
question-and-reply pairs since several runs reproduced earlier replies byte
for byte) through the arm (`tools/answer_relation_arm.py`, the Python side of
the check the C code must agree with). Every phrase the check finds is now
classed by its relation word, kinship or possession (`phrase_class`), and the
arm tallies phrases by class and outcome and replies by which class flagged
them. No model. The before numbers are the committed arm's output classed the
same way (`build/tolerance/relation-arm-all-before.json`, the classing script
in the evidence archive); the after numbers are the arm's own tally
(`relation-arm-all.txt`).

## Before (230 replies, 22 flagged)

| class | phrases supported | negated | flagged |
|---|---|---|---|
| kinship | 66 | 10 | 23 |
| possession | 3 | 8 | **47** |

Replies flagged by a kinship phrase only 11, by a possession phrase only 9,
by both 2.

The kinship phrases do the check's work: the 23 flags are the second novel's
misread parentage ("her father is Tardos Mors", "her mother is Mors Kajak",
nine runs), the third novel's "Heathcliff's nephew" denial, and two first-novel
misreadings ("Mr. Bennet's daughter", "Mr. Bingley's cousin"). Every one is a
wrong relation, as the relation journal judged them.

The possession phrases are flagged 47 times and supported 3. Reading the
flagged sentences (`build/tolerance/relation-poss.txt`):

- **"Mr. Bingley's inheritance"** and **"Bingley's inheritance"** (16 flags
  across the first novel's first question): the composer's own words for the
  passage's "Mr. Bingley inherited property to the amount of nearly a hundred
  thousand pounds". The statement is correct; the check flagged a paraphrase.
  In the retire run this phrase was the reply's only relation flag; that
  reply was withheld anyway for a misquotation (the composer extended the
  quoted passage with words it does not contain), so the live outcome did not
  turn on it. In the first window run the same phrase was flagged in a
  declining reply ("a passage about Mr. Bingley's inheritance").
- **"Mr. Bingley's estate"**, **"Mr. Bingley's estate is Netherfield Park"**
  (one reply, reproduced byte for byte in seven runs): wrong. Netherfield is
  rented, and the estate was never bought. The possession words caught this
  one wrong statement, and no kinship phrase in that reply would have.
- **"Bourgh's fortune"** (the broken retire run only): the regular expression
  stopped the name at "de", so "Miss de Bourgh's fortune" was read as
  "Bourgh's fortune" and not found in the span that states it as "Miss de
  Bourgh, will have a very large fortune". Recorded; the run is excluded from
  the fixture, and the lowercase particle is a name defect for the next
  held-out novel to decide, not this step (no phrase in a kept run has one).

So the possession class, as it stood: one wrong statement caught, one correct
paraphrase flagged in several replies.

## The change

"X's inheritance" is also supported by "X inherited": the one possession word
with a verb, added beside the existing "Y of X" form in `rel_supported` (C)
and `supported` (Python). The other three possession words have no verb and
no rule. The word list is unchanged; nothing leaves the check.

## After (230 replies, 21 flagged; `build/tolerance/relation-arm-all.txt`)

| class | phrases supported | negated | flagged |
|---|---|---|---|
| kinship | 66 | 10 | 23 |
| possession | **13** | 8 | **37** |

Replies flagged by a kinship phrase only 12, by a possession phrase only 8,
by both 1. The one reply that left the flagged set is the retire run's first
reply (the inheritance paraphrase, its only relation flag); the one that moved
from "both" to "kinship only" is the relation run's first reply, still flagged
for "Mr. Bingley's cousin". Of the 8 replies now flagged by a possession phrase
alone, 7 are the "Netherfield Park" reply (wrong) and 1 is the declining
reply whose sources do not state what it says the passage is about (its
sources are "only three miles" and a short-lease line; "Mr. Bingley's
inheritance" is in neither, nor in the question or turns; a decline replaced
by a withheld decline).

On the 103 distinct pairs: possession flags 26 to 11, supported 3 to 18;
replies flagged 9 to 8.

**Parity.** The fixture `tests/fixtures/answer_relation/replies.json` now holds
all 230 recorded replies from the 23 runs (110 from 11 before), with the arm's
result on each; `test_answer_pipeline` (`test_relations`) runs the C check over
every row and agrees on all 230, and `test_answer_relation_arm` re-checks the
fixture from Python; both pin 21 flagged. The unit checks on both sides add
the inheritance paraphrase supported beside the estate phrase still flagged
and a different name's inheritance not supported.

## Decision

The possession class stays in the withholding check. Measured, it has one
catch (a wrong possession no kinship rule reads) and, after the inheritance
rule, no flag on a correct shown reply across 230 recorded replies. The
split is now visible in the arm's output so the next novel's run can read the
two costs apart; it is not carried into the receipt line (`ANSWER RELATIONS`
stays `checked=N | unsupported=<phrase>`), which would change the receipt
contract for a count the arm already gives.

## Live

No live run was taken for this change. The only recorded reply whose
relation outcome changes (the retire run's first) was and remains withheld
for a misquotation; no recorded request changes (the check reads the reply
after the request is built); and the rule touches one word's paraphrase. The
next live run on any novel carries the rule and the arm's split tally.

## Gates

`make test` 95,962 C checks and 174 Python tests, 0 failures (three C checks
and one Python test more than `5f9b53f`: the inheritance cases on both sides,
and the parity fixture grown from 110 to 230 rows); `make analyze` 0 warnings
across 87 source files; `make sanitize` 95,962, 0, 0 diagnostics; `make tsan`
48,592, 0, 0 diagnostics; `make dbtest` 18,000, 0. Every exit status 0
(`build/tolerance/split-gates-*.log`, `split-gates-exit.txt`; the tally sums
every `N checks` line).
