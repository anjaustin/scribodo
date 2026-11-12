# Answer cycle 4: note turns, measured against the checks

**Date:** 2026-11-12. **Tree:** `5f9b53f` plus the relation-split changes.
**Direction:** "Let's wrap up the loose ends. 100/100. Methodically." The
third loose end is the finding `journal/answer-cycle4_heights.md` left for the
author: on the third novel the composer agreed with a wrong note ("Note for
later: Hareton Earnshaw is Heathcliff's son") and embellished it ("Heathcliff's
illegitimate son"); a note turn is not gated, because the claim gate, the
quotation check and the relation check are armed only for a turn that ends
with a question mark (`answer_gate_applies`). The question was whether an
acknowledgement of a note should be checked at all, and against what. This
journal measures what the existing checks would do before anyone decides.

## Method

Every recorded composer reply to a turn that is not a question, from the 23
kept capture directories, de-duplicated on (turn, visible reply): 20 distinct
replies to the fixtures' note turns ("Note for later: …", "Draft note (older):
…") and 12 distinct replies to the fixtures' creative request ("Draft two
sentences, in …'s register, …"). Each reply through the product's quotation
check (`answer_quotations`, via `build/answer_gate_replay`) and the relation
check (the arm, which the C check agrees with on every recorded row), with
the turn's own verified spans, the author's earlier turns and the turn itself
as the sources, exactly as a question turn is checked. No model
(`build/tolerance/note-replies.txt`, `note-checks.txt`, `request-checks.txt`).

## Note turns (20 distinct replies)

| check | replies flagged | what was flagged |
|---|---|---|
| quotation | 2 | Both quote a passage from an **earlier turn's** verified block, not this turn's: the third novel's "Little Hareton was nearly five years old, and I had just begun to teach him his letters" (the manuscript's words, shown two turns before) and the second novel's "Mors Kajak of Helium". |
| relation | 0 | "Heathcliff's illegitimate son" is not a relation phrase to the check: an adjective stands between the possessive and the kinship word, and the four patterns read none. The note itself ("Heathcliff's son") is an author turn and would support the plain phrase anyway. |

So the checks as they stand would withhold 2 of 20 note acknowledgements,
both for quoting text that is in the conversation but not in this turn's
sources, and would not see the embellishment the finding is about. The
embellished reply is one of the two, by accident of its quotation.

Read in full, the 20 acknowledgements also contain, uncaught by any check:
"Lydia (Elizabeth's youngest sister) is seventeen when the story opens …
being married at sixteen" (the note said neither); "This is an earlier detail
from the manuscript (part01.md)" (a draft note taken for manuscript text).
Fourteen of the 20 restate the note and stop, which is what an acknowledgement
should do.

## Creative turns (12 distinct replies)

| check | replies flagged | what was flagged |
|---|---|---|
| quotation | 3 | New writing in quotation marks that is a close paraphrase of the first novel's own line ("I cannot bear to think that they should have all this estate; had it not been for the entail, I should not mind it") and in one reply attributed to "(part03.md, Source 1)" as if quoted. |
| relation | 0 | |

Three of twelve "new writing" replies present altered manuscript text as the
manuscript's, which the quotation check would catch and should; but new
dialogue the composer invents is also written in quotation marks, and the
check would withhold it as a misquotation. Arming the checks on every
non-question turn would therefore gate new writing out of the composer.

## What the product can tell apart

Nothing. A turn is a question when it ends with "?"; a note and a creative
request are both statements to the product. The fixtures' "Note for later:"
and "Draft note (older):" are the fixtures' conventions, not a product
marker, and a check keyed on the author's wording would be a rule about the
author's text, which the project does not write (fix software, not prompts or
data).

## Decision

Note turns stay ungated. The claim gate, the quotation check and the relation
check remain armed on question turns only, where the measured cost is known
and the composer's reply is an answer. What the measurement establishes:

1. The embellishment the finding is about is caught by no existing check and
   would not be caught by arming them.
2. Arming the existing checks on non-question turns would withhold 2 of 20
   note acknowledgements (for quotations from earlier turns) and 3 of 12
   creative replies, and would withhold any invented dialogue in quotation
   marks.
3. The structural fix is an author-declared turn kind: a note the author marks
   as a note (a composer affordance, not a wording), whose acknowledgement the
   product can then check for anything beyond the note and the sources, and
   which the memory record can store as the author's note rather than as a
   chat turn. That is a product decision for the author and is recorded in
   PRODUCT-GAPS as the open item on faithful use.

The composer's agreement with a wrong note is not a corruption of the
author's record: the pipeline's candidates are manuscript units only, so an
assistant turn is never a verified span. It is a model error shown to the
author and not disclosed, and it is now measured.

## Hosted verdicts

No code changed in `ece3439`; CI and Trusted acceptance both passed on it
(runs 43317951921 and 43317951878).
