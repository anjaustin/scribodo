# Answer cycle 4, the P0 list: the third held-out novel

**Date:** 2026-11-12. **Tree:** `4a4aec0` plus the changes recorded below.
**Direction:** "Let's continue with the P0s in order until complete."

## Why a third novel, and which

The first two fixtures each exposed a failure class and were then used: the
first novel's eight questions chose the pipeline's shape across cycles 2 and
3; the second novel's correction case chose the gate's capitalisation rule
(answer-cycle4_mars.md says so and names a third novel as the next held-out
set). The two classes to measure by construction are pronoun-subject
attribute statements (the sisters' fortune, Charlotte's age: the question
names the person, the stating sentence says "they" or "she") and relations
between named people (the one wrong clause shown on the second novel; the
relation check of answer-cycle4_relation.md).

*Wuthering Heights* (Emily Brontë, 1847; Project Gutenberg #768) is built of
both: a nested first-person narration (Lockwood writing down Nelly Dean's
telling) in which nearly every fact about a person is stated with a pronoun
or a demonstrative as its subject, and a family whose relations (cousin,
father, nephew, sister-in-law) are the plot. Its names are English and known
to any tagger, which the second novel's were not; what changes between the
three novels is therefore narration and relation structure, not vocabulary.

## The text and how it enters

Plain text from Project Gutenberg, SHA-256
`e533fe750589f0421d5d744576315f5c2b9b0d69e981179ea0551bbf134c5e02`, 693,819
bytes, imported by `tools/manuscript_import.py` unchanged: 34 chapters,
115,974 words, 661,507 bytes, eight parts of 51 to 117 KB
(`build/manuscripts/wuthering-heights/`, source and checksum beside the
import as for the second novel). The parts are larger than the second
novel's (36 to 50 KB) and comparable to the first's; nothing else differs.

## The held-out rule

The eight questions (`tests/fixtures/answer_boundary/wuthering-heights.json`)
were written on 2026-11-12 from a reading of the imported parts, before any
pipeline or reach run on this novel, and checked mechanically: every
designated passage occurs exactly once in its named part and nowhere else in
the book (the fixture's writer asserts it). They are never tuned against.

By construction:

- three single facts whose stating sentence has a pronoun or demonstrative
  subject while the question names the person: "she was hardly six years
  old" (Catherine), "The former was a boy of fourteen" (Hindley), "her
  husband was her cousin also" (the young Mrs. Heathcliff), the last a
  relation question outright;
- two entities with different values in different parts (Hareton "nearly
  five years old", the younger Catherine "reached the age of thirteen");
- an author's correction about a relation ("Hareton is Heathcliff's son",
  corrected toward "Mr. Hindley Earnshaw, that was Hareton's father");
- contradictory dated drafts over a figure ("a waif for twenty years");
- an unavailable fact (the London street of Lockwood's rooms) and a creative
  request.

`tests/ui/store/23_heights_boundary_questions.scribo` is generated from the
fixture by `tools/answer_boundary_script.py`; `tests/test_answer_boundary_fixture.py`
holds the three scripts to their fixtures and the three fixtures to the
required kinds, fresh questions and complete cases. No reach-task fixture was
written for this novel; the item named the boundary classes, and the reach
measurement is GAP-001's.

## Hosted verdicts

CI passed on `fd5ba05`, `4a4aec0` and `46d65c2`; Trusted acceptance failed on
each at "Run scripted UI acceptance" with the console still locked
(TECHNICAL-DEBT, observed 2026-11-12), the database and crash steps having
passed. `9097df3` failed CI at `make test` (the link error recorded in
journal/gap004-cycle7_atomic.md); `de82f6d` carries the fix. After the console
unlocked, Trusted acceptance passed on `6913d70` (the tree holding every
commit above) and both workflows passed on `0f2e36b` and on `ca908bd` (the live runs and the remediations above).

## Runs

### First run, the relation-check tree (`heights-relation`, 2026-11-12)

The run: `build/answer-live-relation/scribodo` (built from `4a4aec0`), script
`tests/ui/store/23_heights_boundary_questions.scribo`, 30,000 words of the
novel's parts in a disposable database, `SCRIBODO_ANSWER_PIPELINE=verified`,
trace on, excerpts off, seed 42; 576 s, 10 questions, 10 receipts, 40 capture
files (`build/product-memory-use/heights-relation`,
`build/relation/capture-heights-relation`). The fixture's 27 expectations:
23 passed, 4 failed, every failure a designated passage absent from the
receipt. No question had been asked of this novel before; the questions were
written from the text before the run (§The held-out rule).

#### Selection

| Case | Designated unit rank (score) | In a slot | Note |
|---|---|---|---|
| Catherine rides ("hardly six years old") | 1 (0.986) | yes | |
| Hindley's age ("a boy of fourteen") | **5** (0.907) | no | top 0.941 and 0.939: two part02 units about Hindley, neither with the age |
| cousins ("her husband was her cousin also") | **4** (0.946) | no | top 0.987: the "may be her husband: Heathcliff junior" passage; the one that states the relation ranks under it |
| two ages (pronoun subjects) | 1 (0.998) and 2 (0.998) | both | both passages say "she" and "Little Hareton"; selection reached them |
| author correction ("that was Hareton's father") | **not a candidate** (0 of 59) | no | the two slots held "Not my son, assuredly." and "the late Mrs. Linton's nephew", which answer the question in part |
| contradictory drafts ("a waif for twenty years") | 1 (0.980) | yes | |
| creative ("atmospheric tumult") | **13** (0.688) | no | |

Five of eight designated units reached a slot; three selection misses and one
unit the retrieval never produced. The pronoun-subject case, written to probe
exactly that, was reached without the index layer.

#### Replies

| Case | Gate | Outcome |
|---|---|---|
| Catherine rides | pass, shown | correct: quotes "hardly six years old", answers "nearly six" |
| Hindley's age | pass, **withheld** (misquotation) | the composer answered "sixteen" from a passage about Heathcliff and, in a parenthesis, quoted "hardly six years old" from the previous answer, words not in this question's passages; a wrong answer withheld, for the misquotation rather than the error |
| cousins | not_in_span, **withheld** | the composer named "Heathcliff junior" and added "threw herself into the sea", which no passage says; the misquotation reason took precedence (see below) |
| two ages | not_in_span, **withheld** | **a correct answer withheld**: "Hareton is nearly five years old; the younger Catherine was under thirteen". The passages say "she"; the name "Catherine" is in the author's question and nowhere else, and a mid-sentence name is a hard anchor (see below) |
| note (Hareton is Heathcliff's son, a wrong note by construction) | no gate | the composer agreed with the wrong note and added "illegitimate son"; a note turn is not gated, and the relation check would count the author's own turn as support |
| author correction | no_answer, **withheld** (relation) | the composer wrote "Hareton Earnshaw is Heathcliff's nephew" (wrong; the passage says the late Mrs. Linton's nephew) and asked back "Who is Hareton Earnshaw's father?"; the check flagged the question's phrase, not the wrong "nephew" (see below) |
| older-draft note | no gate | acknowledged weakly ("I don't have the earlier context") |
| contradictory drafts | no_answer, shown | quotes "twenty years" from the current text, then says it has not enough of the manuscript; the figure is right and the hedge is not |
| unavailable fact | no_answer, shown | correct abstention ("do not mention a London street") |
| creative | no gate | two sentences marked "New writing"; the designated passage was not among the sources |

Reach: 5 of 8 designated units in a slot. Faithful use: of the 7 gated turns,
3 shown and correct (two abstentions and one answer), 3 withheld wrong or
unsupported answers, 1 withheld correct answer. No wrong relation and no
misquotation was shown.

#### What this run says about the checks (red team of the step)

- **The gate withholds a correct answer whose name is only in the question.**
  The two-ages case: the composer named the entity the passage calls "she", as
  the author did in the question; the anchor check looks in the passages and
  the earlier turns, not the current question, except for a sentence-initial
  name. Recorded in `journal/answer-cycle4_relation.md` with the measurement
  of the candidate rule.
- **A nested quotation mark counts as a misquotation.** The cousins reply
  quoted the passage exactly except that the inner “…” became '…'; the
  normalizer keeps apostrophes, so the quote failed. In this run the reply was
  wrong anyway and the verdict right; the reason was not.
- **The relation check flagged a question the composer asked back** ("Who is
  Hareton Earnshaw's father?") and let a wrong statement pass unexamined
  ("Hareton Earnshaw is Heathcliff's nephew"): questions are not statements,
  and "nephew" is not in the kinship list. The withhold was right by accident.
- **Note turns are not gated.** The composer agreed with a wrong note and
  embellished it. The relation check would not have helped: the author's own
  turn states the relation, and the author's turns are support by design. A
  decision for the author: whether an acknowledgement of a note should be
  checked at all, and against what. Measured on 2026-11-12 over every
  recorded note and creative reply: `journal/answer-cycle4_note_turns.md`
  (the existing checks would not see the embellishment and would withhold
  new writing; the structural fix is an author-declared turn kind).

The first three are software, measured and remediated in the relation record;
the fourth is recorded as a finding.

## Reach (selection only, production planner; 2026-11-12)

The fifteen document-fact tasks
(`tests/fixtures/memory_reach/wuthering-heights-tasks.json`) were written on
2026-11-12 from the candidate sentences of the eight imported parts (a name and
a number in one sentence, 60 to 170 characters), before any reach run on this
novel, two per part and one for the sixth; each required text is the part
file's own bytes and occurs once in the book, and the fixture's writer asserted
both. Then `tools/memory_reach_eval.sh --words 30000 --per-category 3
--manuscript build/manuscripts/wuthering-heights/parts --manuscript-tasks
tests/fixtures/memory_reach/wuthering-heights-tasks.json` at `2aede34`, the
same shape as the first two novels' runs (the 30K fixture chat with the eight
parts attached, real tokenizer and embedder, 16 lexical and 16 semantic
candidates, 8 admitted excerpts), on an idle machine after the day's gates. Run
`build/memory-reach/runs/scribodo_test_reach_84012_dda94ab4a2395173`, 129 s;
all harness gates passed; the first novel's control at the same tree
(`build/memory-reach/runs/scribodo_test_reach_91611_a4a3f43632da1dd4`, all
gates passed) is unchanged from its two earlier runs to the last digit (the
same four misses), so the planner has not drifted between measurements.

| Novel | Document tasks | reach@candidates | reach@admitted | lexical hit | semantic hit | planning p95 |
|---|---:|---:|---:|---:|---:|---:|
| Pride and Prejudice (control at `2aede34`, `..._91611_...`, 131 s) | 14 | 1.00 | 0.71 | 0.93 | 0.93 | 137 ms |
| A Princess of Mars (2026-11-11, `..._63447_...`) | 15 | 1.00 | 1.00 | 1.00 | 0.80 | 108 ms |
| Wuthering Heights (`..._84012_...`) | 15 | 0.93 | **0.80** | 0.93 | 0.73 | 157 ms |

Twelve of fifteen held-out passages were admitted. The three that were not:

- **t28**, Isabella's age at the window ("Isabella—I believe she is eleven, a
  year younger than Cathy—lay screaming at the farther end of the room"): not
  among the 32 candidates by either channel. The question names Isabella
  Linton, the window and Thrushcross Grange; the sentence has "Isabella",
  "eleven" and "Cathy" and nothing else the question says.
- **t29**, the three days of old Mr. Earnshaw's absence ("It seemed a long
  while to us all—the three days of his absence—and often did little Cathy
  ask when he would be home"): a candidate by both channels, not admitted. The
  subject is "his"; the question names Mr. Earnshaw, Liverpool and Heathcliff.
- **t39**, the hour Heathcliff came ("At seven o’clock he came, and inquired
  if Miss Linton had risen"): a candidate by both channels, not admitted. The
  subject is "he".

All three are the first novel's miss class, an attribute or event whose
sentence names its subject by a pronoun while the question names the person:
the class LANDSCAPE row 12 recorded as "not yet measured on a second book".
It is now measured on a third: the second novel's fifteen tasks had no such
sentence and reached 1.00; this novel's fifteen have three and reach 0.80. The
difficulty is the question's distance from the name, not the book, which is
what the second novel's record said. Nothing in the selection path was changed
for this novel, and the fixture is not tuned against.


### Hosted verdicts for the reach step

CI passed on `f2558ea` (run 43322785218); its Trusted acceptance run
(43322785148) was cancelled by the next push, and Trusted acceptance passed on
`a8b8da1` (run 43323008199), which carries the same code and the evidence
record.

CI passed on `a8b8da1` as well (run 43323008274).
