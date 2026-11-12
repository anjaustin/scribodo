# Answer cycle 4, the P0 list: the relation check on the reply

**Date:** 2026-11-12. **Tree:** `fd5ba05` plus the changes recorded below.
**Direction:** "Let's continue with the P0s in order until complete."

## The item

Every wrong statement shown or withheld on either novel at this tree is a
relation between sourced words: "sons" for "daughters" (inside a quotation,
which the quotation check catches), two draft labels swapped before a right
conclusion, Elizabeth read for Charlotte, and on the second novel the one
wrong clause an author saw, "her father is Tardos Mors and her mother is Mors
Kajak", written from a span that says "the daughter of Mors Kajak, son of
Tardos Mors". The claim gate reads names and numbers; the quotation check
reads quoted text; neither reads a relation the composer states in its own
words. Cycle 4's window record left the question open: can a deterministic
check hold "X of Y" to the span without a model?

## The arm

`tools/answer_relation_arm.py` (test `tests/test_answer_relation_arm.py`, in
`make test`, model-free) measures one candidate on recorded replies. The
check reads the reply's prose, with quotations and blockquote lines removed
(the quotation check's territory), sentence by sentence, and finds kinship
and possession phrases of four shapes: "<kin> of <Name>", "<Name>'s <kin>",
"<his|her|their|Name> <kin> is <Name>" and "<Name> is the <kin> of <Name>",
with kin from a closed list (father, mother, son, daughter, sister(s),
brother(s), wife, husband, friend, estate, fortune, inheritance, property)
and Name a capitalized run, honorific allowed, sentence starters and the
composer's labels ("The", "Source", "Summary") excluded. A phrase is
supported when its normalized text (the citation classifier's folding) is a
substring of a verified span, an author turn or the question; "X's Y" is also
supported by "Y of X". A sentence carrying a negation (not, never, neither,
nor, wrong, instead, isn't, aren't, wasn't) is skipped: a correct correction
states the wrong relation in order to deny it, and the recorded correction
does exactly that ("she is not the daughter of Tardos Mors").

The arm's own corrections during the run, each a rule the C check inherits:
"parentage" and "age" left the kin list (they flagged a note's paraphrase,
"Dejah Thoris's parentage", which is no relation between two names); an
honorific's period does not end a sentence ("Mr. Bingley's" was being split);
a sentence-initial pronoun is a subject ("Her father is").

## Measurement on recorded replies

Six captured runs, 60 replies (`build/window/capture-pp`, `capture-pp3`,
`capture-mars`, `-mars2`, `-mars3`, `-mars-receipt`; output
`build/relation/rel3.txt`, `rel3.json`):

| Flagged reply | Runs | Unsupported phrase | The journals' judgment of the reply |
|---|---:|---|---|
| Second novel, correction | 4 of 4 | "her father is Tardos Mors", "her mother is Mors Kajak" | the one wrong clause shown to an author on either novel (answer-cycle4_mars.md) |
| First novel, fortunes | 2 of 2 | "Mr. Bennet's daughter" (window1), "Mr. Bingley's estate" (window2) | the Bennet misattribution; the estate-for-inheritance claim; both withheld for a misquotation already |
| First novel, walk (window1) | 1 of 1 | "Mr. Bingley's inheritance" | an abstention whose prose invents what the "only three miles" passage is about |

7 of 60 replies flagged; every flag is a relation the sources do not state;
no correct shown answer is among them. The correct denials in the correction
reply ("not the daughter of Tardos Mors") are skipped as negated, and the
right relations beside them ("daughter of Mors Kajak", "son of Tardos Mors")
are supported. Not reached by this check, and recorded as its limits: the
drafts' swapped labels (no kinship or possession), Charlotte read as Elizabeth
(a pronoun subject, an abstention), and anything inside a quotation, which is
the quotation check's.

## The product

`answer_relations` (`src/llm/answer_pipeline.c`) is the C mirror of the arm:
the same prose rule, sentence rule, patterns, name filter, support rule and
negation list, hand-written (no regular expressions in the product). The
fixture `tests/fixtures/answer_relation/replies.json` holds the 60 recorded
replies with their sources and the arm's result for each (phrases counted,
first unsupported phrase); `test_answer_pipeline` (`test_relations`) runs the
C check on every row and requires the same count and the same first
unsupported phrase, 60 of 60, beside unit checks of the misreading, its
denial, the quotation exclusion, the "of" form of a possessive, the author
turn as a source and the honorific period. `test_answer_relation_arm`
re-checks the same fixture from the Python side, so a change to either must
regenerate the fixture and satisfy both.

`gate_apply` (`chat.c`) runs the check after the quotation check: a reply
with an unsupported relation is withheld with the reason "it states a
relation the verified passages do not" (a misquotation keeps its own reason
when both occur). The receipt gains a third gate line,
`--- ANSWER RELATIONS | checked=<n> | unsupported=<text> ---`, and the trace
line gains `relations=<n> unsupported=<0|1>`; `src/llm/answer_receipt.h` is
updated as the contract prescribes, the fixtures regenerated, the reader
tests and the review tool extended; the store bound reserves room for three
fields (`ANSWER_RECEIPT_GATE_RESERVE`), cut in order claim, misquotation,
relation. The composer window's refusal rule is unchanged (withheld=1).

What this would have done on the recorded runs is the measurement above:
the second novel's correction turn becomes a withheld reply with the
passages shown, and no correct answer is lost. On the first novel the two
flagged fortunes replies were already withheld.

## Live measurement

Pending: the console is locked on this Mac (TECHNICAL-DEBT, observed
2026-11-12), so neither novel's boundary script can run live now. The live
run of both novels on this tree, compared with `pipe-window2` and
`mars-pipe7` on requests, replies and receipts (unit ids masked), is the next
step once the console is unlocked; the offline arm already states what the
gate decides on the recorded replies, and a live run adds the composer's
variance, not the gate's.

## Corrections during the step

Two defects of my own, found by the analyzer and by reading the C scanner
again before the gates: the prose buffer is now zero-initialized (the static
analyzer saw a path reading an undefined byte), and the third pattern lost the
object's start when its subject was a name rather than a pronoun; no recorded
reply has such a phrase, so the parity fixture did not see it, and a unit
check on both sides now pins the path ("Elizabeth father is Mr. Bennet").

## Hosted verdicts

CI passed on `fd5ba05`, `4a4aec0` and `46d65c2`; Trusted acceptance failed on
each at "Run scripted UI acceptance" with the console still locked
(TECHNICAL-DEBT, observed 2026-11-12), the database and crash steps having
passed. `9097df3` failed CI at `make test` (the link error recorded in
journal/gap004-cycle7_atomic.md); `de82f6d` carries the fix. The acceptance
jobs are rerun once the console is unlocked.

## Gates

`make test` 95,901 C checks and 167 Python tests, 0 failures; `make dbtest`
18,000, 0; `make analyze` 0 warnings across 88 source files; `make sanitize`
95,901, 0, 0 diagnostics; `make tsan` 48,565, 0, 0 diagnostics. Every exit
status 0 (`build/relation/gates3-*.log`; the first run's analyzer finding and
its fix are in `gates-analyze.log` and `gates2-analyze.log`).
