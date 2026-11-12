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

## Runs

None yet: the console is locked on this Mac (TECHNICAL-DEBT, observed
2026-11-12), so no live run can open a window. The first run is staged with
the relation-check tree's live-capture app (`build/answer-live-relation`),
after the two earlier novels' runs on the same tree, and its results go in
this record beside the second novel's first-run table.
