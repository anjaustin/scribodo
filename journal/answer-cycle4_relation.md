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

The console unlocked on 2026-11-12 and the three novels ran live on the
relation-check tree's capture app (`build/answer-live-relation`, built from
`4a4aec0`; the capture app's instrumentation records every request and reply,
MODEL-002), each in its own disposable database, under
`SCRIBODO_ANSWER_PIPELINE=verified`, `SCRIBODO_MEMORY_TRACE=1`,
`SCRIBODO_MEMORY_EXCERPTS=0`, seed 42. A first attempt was worthless and is
kept as `build/product-memory-use/*-relation-nocapture`: the capture
directories had not been created, so the capture app refused every question
("Live capture requires a private owned directory and local inference"), and
the runs finished normally with no receipts and no trace. The requirement is
now a memory note and the directories are created `0700` before a run.

The comparison masks unit ids and timings and compares score pools as sets
(`journal/answer-cycle4_receipt.md`, calibration 10/10/10/27 on
`mars-pipe6` against `mars-pipe7`; `build/relation/*-compare.txt`). The new
receipt line and the two new trace fields are removed before the receipt and
gate-line comparison, since every receipt gains them by construction.

### The second novel: `mars-relation` against `mars-pipe7`

| | mars-pipe7 (before, 323 s) | mars-relation (after, 352 s) |
|---|---|---|
| Requests byte-identical | 10 | **6 of 10**: questions 1 to 6; 7 to 10 follow a changed reply in their history |
| Replies byte-identical | 10 | **6 of 10**, the same six |
| Receipts identical, unit ids masked, relation line removed | 10 | 6 of 10 (the first differs only in the candidate pool counts in its header, see below) |
| Trace lines identical, relation fields removed | 27 | 16 of 27 |
| Replies withheld by the relation check | 0 | **1**: question 6 |
| Fixture expectations | 25 passed, 2 failed (the two source-delivery checks, `journal/answer-cycle4_mars.md`) | 25 passed, the same 2 failed |

Question 6 is the author-correction case ("Earlier I told you Dejah Thoris is
the daughter of Tardos Mors; I was wrong, the text says otherwise. Whose
daughter does she say she is?"). Before, the composer's reply quoted the right
passage and then stated "her father is Tardos Mors and her mother is Mors
Kajak", the one wrong relation shown in any recorded run (§Measurement on
recorded replies). Now the gate checks seven relation phrases in that reply,
finds "her father is Tardos Mors" in no passage, turn or question, and the
author sees the withheld notice with both verified passages (Source 2:
"I, the daughter of Mors Kajak, son of Tardos Mors"). The receipt's third line
reads `checked=7 | unsupported=her father is Tardos Mors`; the gate verdict
stays `pass` with `withheld=1`.

Every reply before that question is byte-identical to the recorded run: the
check changed nothing it was not built to change. Every request after it
differs, because the withheld notice is now in the history the composer sees,
and so the four later replies differ. Three are equivalent in kind (the
pump-count lookup quotes the same Source 1; the horse question abstains in
both; the creative request writes new prose in both). One is worse: the older
draft note ("Draft note (older): In the first draft the atmosphere plant had
ten radium pumps") was acknowledged before ("Got it — noted for the older
draft") and is now answered as a lookup ("No verified passage was found … I
cannot confirm or correct that detail"). That is the composer's sensitivity to
its own history, not the gate's doing, and it is recorded here as the live
cost of the step: one note turn in ten, after a withheld reply, lost its
acknowledgement. The offline arm cannot see this; only a live run can.

The first question's receipt and the two score lines differ with nothing else
different: `candidates=52 hydrated=47` before, `candidates=49 hydrated=48`
after, with the same top two units, scores and reply. The unit exports explain
it: the manuscript units are the same set in both runs (0 differ), and the
four units present only in the earlier run are chat turns (the author's note,
the correction question, two replies) that the memory ingest had stored by the
time of the export. Chat turns enter memory asynchronously, so the candidate
pool at a question depends on how many had been ingested by then; they never
reached the top two in any recorded run. The calibration pair happened to
agree on this; the measure's "trace lines identical" claim therefore counts
pool size as a known source of variance, and the comparison of what matters,
requests and replies, is unaffected.

### The first novel: `pp-relation`, and why its baseline was wrong

`pp-relation` (399 s) completed 10 questions with receipts and trace. The
intended baseline `pipe-window2` turned out to have run with the index layer
on (`SCRIBODO_INDEX_TEXT_FILE`, 10 `ANSWER_INDEX_TEXT enabled=1` lines; the
window record used the coref arm's sidecar), while the relation run had it
off, the product default. So the two differ at the first request (candidates
58 against 59, hydrated 35 against 34, a different second unit) for a reason
that has nothing to do with the relation check, and 0 of 10 requests match.
Its own result stands as a run: 23 expectations passed and 4 failed, the two
source-delivery checks the window record discusses plus two more (the entail
and the fortunes receipts) that the index layer had carried. No reply was
withheld by the relation check (`unsupported=0` on all seven gate lines). The
like-for-like run, `pp-relation-index`, with the same sidecar as
`pipe-window2`, is recorded below.

### The first novel, like for like: `pp-relation-index` against `pipe-window2`

Same sidecar (`build/cr2/product-fcoref_replace_all/index_text.json`, 403
units), same script, seed and flags; 518 s.

| | pipe-window2 (before) | pp-relation-index (after) |
|---|---|---|
| Requests byte-identical | 10 | **10 of 10** |
| Replies byte-identical | 10 | **10 of 10** |
| Receipts identical, unit ids, relation line and pool counts masked | 10 | **10 of 10** |
| Replies withheld by the relation check | 0 | 0 (the fortunes reply carries `relations=9 unsupported=1`, and was already withheld for a misquotation) |
| Fixture expectations | 25 passed, 2 failed | 25 passed, the same 2 failed |

On the first novel the check changed nothing the author sees. The receipts
differ only in the candidate-pool counts of their headers, the chat-unit
variance described above; with those masked, every receipt is identical.

### The third novel: `heights-relation`

First run of the held-out novel, recorded in full in
`journal/answer-cycle4_heights.md` (selection, replies, red team): 10
questions, 576 s, 23 of 27 fixture expectations. For this record: the relation
check withheld one reply (the author-correction case), for a reason that was
right by accident, and the run exposed four defects of the checks, below. No
wrong relation and no misquotation was shown to the author in any of the three
runs.

## Red team of the live runs: four defects, remediated and measured

The Wuthering Heights run, the first text the checks had never seen, found
defects in the relation check, the quotation check and the gate. Each is
software; each was remediated in the C check and its Python mirror together
and measured on every recorded reply before it went in: the 90 captured
composer replies of nine runs on three novels (the six captures of the
60-reply fixture plus the three new runs), the gate replay over 126 recorded
claims (`tools/answer_gate_replay.py`, every review sheet including the three
new runs' and `mars-pipe7`'s), and the product's quotation check over the 90
visible replies against the misquotation each run's receipts recorded.

**The arm measured text the product never checks.** The capture's
`content-N.txt` is the composer's raw output; 13 of the 90 carry a
`<think>…</think>` deliberation that the product strips
(`llm_split_thinking`) before showing or checking a reply. On the raw text the
arm flagged 12 of 90 replies, three of them for phrases in a deliberation the
product's receipts show as `relations=0`. The arm now strips the block exactly
as the product does (`visible()`, with the splitter's orphan-close rule, pinned
by a test): 9 of 90, and every flag a phrase the author saw.

**A question the composer asked back counted as a statement.** "Who is Hareton
Earnshaw's father?" was the flagged phrase in the Heights correction reply,
while the wrong statement beside it, "Hareton Earnshaw is Heathcliff's nephew",
went unchecked: "nephew" was not in the kinship list, and the sentence's "not
his son" negated the whole sentence under the sentence-level negation rule. Three
rules changed: a sentence containing "?" states nothing; the kinship list
gained nephew, niece, cousin(s), uncle, aunt, grandfather, grandmother,
grandson, granddaughter, widow and heir; a negation reaches its own clause
(clauses split at `, ; :` and "but"), so "X is Y's nephew, not his son" states
the first and denies the second. Measured on the 90: 9 flagged before the
clause rule, 10 after; the one reply added is the Heights correction, now
flagged for "Heathcliff's nephew". The clause rule also un-negated supported
phrases in the five Mars correction replies ("daughter of Mors Kajak" after the
semicolon), which stay unflagged because the passage states them, and one
correct paraphrase in a reply already flagged for a wrong relation ("Mr.
Bingley's inheritance", which the passage states as "inherited … nearly a
hundred thousand pounds"): the possession words keep the precision cost they
had when they were added. No reply that was shown and correct is flagged.

**Two name defects found while mirroring.** "daughter of Mr. Bingley's cousin"
matched as "daughter of Mr. Bingley" (a wrong phrase from a wrong reply, so the
flag was right and the phrase was not), and "She is the daughter of X" took
"She" for a name. A pattern whose final name is followed by a possessive is no
match, and the pronouns and question words joined the non-name list. 60
phrases checked across the 90 replies, 10 flagged, before and after.

**A nested quotation mark counted as a misquotation.** The Heights cousins
reply quoted its passage exactly except that the passage's inner “…” became
'…'; the normalizer kept quotation marks, so the quote failed and the reply was
withheld for the wrong reason (the right one, `not_in_span`, was next in
line). Quotation marks are now not words: double quotes are dropped and a
single quote stays only as an apostrophe inside a word (`answer_normalize`
and the Python `normalize` the arms share). Over the 90 visible replies the
product's check now reports 8 misquoted, the runs' receipts recorded 8: the
cousins reply's false misquotation is gone, and the one difference the other
way is the Heights note turn, which the product does not gate and the replay
does, quoting the previous turn's passage. No quotation that was shown changed
status.

**The gate withheld a correct answer whose name was only in the question.**
The two-ages case: both passages say "she"; the author's question names "the
younger Catherine"; the composer's correct claim used the name mid-sentence,
where it was a hard anchor found in no passage. A word the author's own
question contains is now a soft anchor wherever it stands (the earlier rule
allowed it only at a sentence start). The gate replay over 126 recorded claims
changed two verdicts, both on the Heights run: two-ages `not_in_span → pass`
(the correct answer is shown) and cousins `not_in_span → echoes_question` (the
fabricated reply stays withheld; its only anchors are names the question
holds). Every other recorded verdict is unchanged. As with the second novel's
capitalisation rule, this tunes the gate on the held-out fixture, so the third
novel no longer holds out the gate (LANDSCAPE row 13 says so); the next novel
does.

**Recorded, not remediated.** A note turn is not gated, and the composer agreed
with a wrong note and embellished it ("Heathcliff's illegitimate son"). The
relation check would not help as it stands: the author's own turns are support
by design, and the note states the relation. Whether an acknowledgement should
be checked, and against what, is the author's decision
(`journal/answer-cycle4_heights.md`).

The parity fixture `tests/fixtures/answer_relation/replies.json` now holds the
90 visible replies (10 flagged); `test_answer_pipeline` (`test_relations`) and
`tests/test_answer_relation_arm.py` agree on every row and pin the new rules
with unit cases; `test_answer_gate` pins the question-name rule;
`test_normalize` the quotation marks. Arm outputs: `build/relation/rel4-before`
(raw text), `rel4-after` … `rel4-after6` (each rule in turn); gate replay
`build/relation/gate-replay-before.json` and `gate-replay-after2.json`;
quotation replay `build/relation/quote-replay-after.txt`.

### Found live on the first novel after the remediations: a sixth defect

The builder step's live runs (`journal/answer-cycle4_builder.md`) were the
first runs of the tree with the five remediations above, and on the first
novel they withheld a correct answer that every earlier tree had shown. The
author-correction question ("How old is my youngest Bennet daughter?") got the
right reply, "The author's youngest Bennet daughter is Lydia, and she is
**fifteen**", and the relation check flagged "Bennet daughter is Lydia": the
third pattern accepted a bare name as the subject of "<subject> <kin> is
<Name>", and "Bennet" there is the family name used as an adjective, not a
possessor. The pattern now takes a pronoun or a possessive name
("Lydia's father is Mr. Bennet", checked whole with both names); the bare-name
form matched nothing grammatical. Measured on 110 recorded visible replies (the
90 above plus the builder step's two runs): 13 flagged before, 12 after; the
one reply released is this one, and the one phrase added, "Mr. Bingley's
estate is Netherfield Park" in a reply already flagged, is wrong. The parity
fixture now holds the 110 (12 flagged); the unit cases on both sides changed
from the bare-name form to the possessive one. Confirmed live: on the final tree
(`pp-final-index`) the same reply is shown, `withheld=0`, `relations=0`
(`journal/answer-cycle4_builder.md`, the final run).

How the reply came to differ at all is worth recording. On the relation tree
(`pp-relation-index`) the same question's second verified span was one
sentence with status `verified_prefix`: the extraction model had returned two
sentences whose quotation marks did not match the unit's, so the byte check
failed and the longest sentence prefix was kept. Under the quotation-mark rule
above, the same two sentences verify whole, the composer saw a longer passage,
wrote a different reply, and that reply held the phrase. Two things follow.
The quotation-mark rule changes what the author is shown as a verified span,
not only what the quotation check accepts, and the span shown is the model's
text, with its quotation marks, not the unit's bytes: that was already so for
case, emphasis, dashes and whitespace, and is now so for quotation marks. A
verified block that displays the unit's own bytes at the matched position
would make "exact spans" exact by construction; that is a change to the
extraction's output, recorded here as the next item for the author's decision.
And a live baseline must be built from the parent commit: the relation tree's
capture app was the wrong baseline for the builder step for the same reason
the window run was the wrong baseline above.

### Gates for the remediations

`make test` 95,943 C checks and 168 Python tests, 0 failures; `make dbtest`
18,000, 0; `make analyze` 0 warnings across 88 source files; `make sanitize`
95,943, 0, 0 diagnostics; `make tsan` 48,572, 0, 0 diagnostics
(`build/relation/gates5-*.log`, every exit status 0; the tally sums every
`N checks` line, the prefixed one included). The gate replay and the quotation
replay were rerun on the final build (`gate-replay-after2.json`,
`quote-replay-after.txt`) with the results stated above.

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
journal/gap004-cycle7_atomic.md); `de82f6d` carries the fix. After the console
unlocked, Trusted acceptance passed on `6913d70` (the tree holding every
commit above) and both workflows passed on `0f2e36b` and on `ca908bd` (the live runs and the remediations above).

## Gates

`make test` 95,901 C checks and 167 Python tests, 0 failures; `make dbtest`
18,000, 0; `make analyze` 0 warnings across 88 source files; `make sanitize`
95,901, 0, 0 diagnostics; `make tsan` 48,565, 0, 0 diagnostics. Every exit
status 0 (`build/relation/gates3-*.log`; the first run's analyzer finding and
its fix are in `gates-analyze.log` and `gates2-analyze.log`).
