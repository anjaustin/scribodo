# Answer cycle 2 — ranked-span arm (predeclared)

**Date:** 2026-11-10. **Question:** the extraction arm showed this model cannot
select (boolean 1 to 2 of 9; span-only copies something from every unit) and
that the composer reads one correct span well and loses it among nine. Does a
deterministic specialist, the production embedder, recover the designated span
at the top of the verified list, and does composing from the top one or two
spans recover entail, the fortunes and Charlotte without losing the three
abstentions?

**Harness fixes carried in (from run 3's defects):**
- No character cap on the span schema (`--variant span_uncapped`); when a
  copied span runs past the unit, software keeps the longest prefix ending at a
  sentence boundary that is verbatim in the unit (`verified_prefix`), else rejects.
- Separate factual and creative composers. The creative composer is used for
  the fixture's `creative_speculative` case only, chosen by the fixture's kind.
  In production this would need a request classifier; here it is a harness
  decision recorded as such.

**Ranking:** Qwen3-Embedding-0.6B (pinned `421a27e5…`), the production server
flags (`--embedding --pooling last -fa off`, 2,304-token context and batch),
the production query instruction prefix on the question, the production
2,000-byte clip, L2 normalization, cosine. Only byte-verified spans are ranked.
No threshold: top 1 and top 2 are composed; scores are recorded so a threshold
can be studied later, not applied now.

**Inputs, model, seeds:** as runs 1 to 3 (`build/ux1..3`). Output `build/ux4/`.

## Arms per case

| Arm | Composer input |
|---|---|
| `compose` | all verified spans from delivered units (run 3 shape, new composer) |
| `compose_top1`, `compose_top2` | the top 1 / 2 ranked verified spans from delivered units |
| `compose2_top1` | `compose_top1` repeated: byte-identical or not |
| `compose_added` | all verified spans, delivered plus the designated unit retrieval missed (4 cases) |
| `compose_added_top1`, `compose_added_top2` | ranked over delivered plus added |

Reply checks are the predeclared tables from the extraction arm (`CHECKS`,
`CHECKS_ADDED`); every mechanical pass is also read by hand, as run 3 showed
a pass on the word with the wrong gloss.

## What would count

- **Rank of the designated span** among verified spans, per case: 1 is the
  claim. Entail, both fortunes, Charlotte in the realistic pool; three miles,
  "not sixteen", entail (creative) in the added pool. If the embedder does not
  put the stating span first on most of these, the specialist has to be a
  different component (cross-encoder or extractive reader), and that is the
  finding.
- **Composition from top 1 or 2** passing on entail, fortunes and Charlotte by
  hand, with the three abstentions (walk, drafts realistic; unavailable fact)
  intact. An abstention lost to a confidently ranked irrelevant span is the
  failure mode to watch: with no threshold, top 1 always exists when any span
  verified, so the composer must still say the span does not answer.
- Zero "New writing" on factual replies and zero markers, confirming the two
  harness fixes.
- `expected_true_verified` 9 of 9 now that the cap is gone, with the one
  prefix cut counted separately.

Results follow below once the run completes; nothing above is edited after it.

## Results (`build/ux4/`, 2026-11-10)

99 model calls (58 extraction, 41 composition) plus 8 query and 44 span
embeddings; 332 s of LFM3 time. One extraction call hit its 256-token cap (an
uncapped span copied past the unit and was rejected). `compose2_top1` was
byte-identical to `compose_top1` on 7 of 7. Zero scaffolding markers and zero
"New writing" lines on factual replies: both harness fixes held.

### Extraction without the cap

| | run 3 (600-char cap) | run 4 (uncapped, sentence cut) |
|---|---:|---:|
| Spans verified | 42 | 29 |
| …by sentence-prefix cut | n/a | 0 |
| Spans rejected by the byte check | 16 | 27 |
| Expected-true units verified (of 9) | 8 | **9** |
| Expected-false units with a verified span (of 49) | 34 | 20 |

Without the cap the model paraphrases more often ("Mr. Bennet's estate is
entailed to his poor girls…") and the byte check removes it. The sentence cut
never fired: a rejected span was never a correct sentence with extra text
after it. All nine stating spans verified, including the 680-character
"girl of fifteen" paragraph that the cap had broken.

### Ranking by the production embedder

| Case | Pool | Designated span | Rank | Score |
|---|---|---|---:|---:|
| single_fact_entail | 3 realistic | entail sentence | **1** | 0.794 |
| two_entities_fortunes | 5 realistic | "nearly a hundred thousand pounds" | **1** | 0.758 |
| two_entities_fortunes | 5 realistic | "fortune of twenty thousand pounds" | 4 | 0.417 |
| single_fact_charlotte_age | 1 realistic | "about twenty-seven" | **1** | 0.595 |
| author_correction | 2 realistic | "girl of fifteen" | **1** | 0.523 |
| single_fact_walk_distance | 3 added | "only three miles. I shall be back by dinner." | 3 | 0.300 |
| author_correction | 3 added | "my youngest is not sixteen" | 3 | 0.321 |
| contradictory_drafts | 8 added | "only three miles. I shall be back by dinner." | 5 | 0.294 |
| creative_request | 1 added | entail sentence | 1 | 0.532 |

First place on 4 of 5 stating spans in the realistic pools. The misses are the
short spans: the two 44-character "only three miles" spans and the two
spans about the sisters and the youngest daughter whose wording does not
restate the question's terms. The production query instruction is written
for statements ("Given a statement from a work of fiction, retrieve
established facts…"), not questions; this was used as-is for parity and is
the obvious variable to measure next, old against new, before anything else
is read into these ranks.

### Composition

Mechanical: all-spans 7 of 8, top 1 **6 of 7**, top 2 5 of 7; added 3, 2, 2
of 4. By hand:

| Case | top 1 | By hand |
|---|---|---|
| single_fact_entail | pass | quotes the entail sentence verbatim and cites it; the gloss is muddled ("passes only if there are male heirs, in default of a distant relative"). Top 2's gloss is closer. Correct quotation, unreliable paraphrase, both arms. |
| two_entities_fortunes | fail (mechanical) | **correct for its input**: states the hundred-thousand inheritance and says the sisters' fortune is not in the supplied text. The sisters' span ranked 4th and was not supplied. All-spans and top 2 both stitched a false relation ("Mrs. Jenkinson's nieces, Bingley's sisters"; "his sister Miss de Bourgh"). |
| single_fact_charlotte_age | pass (mechanical) | **fails by hand, third run running**: reads "They had several children. The eldest of them… about twenty-seven, was Elizabeth's intimate friend" as Elizabeth's age and abstains. The inference "the eldest Lucas child is Charlotte Lucas" is not made. |
| single_fact_walk_distance | pass | abstains correctly with an irrelevant top span. Added arm: with all three spans it answered "about three miles" correctly; with the top 1 or 2 the stating span was ranked out and it abstained. |
| author_correction | pass (mechanical) | quotes "girl of fifteen" and still declines to conclude: "there is no mention of the youngest Bennet daughter's age". The link Lydia = youngest, given in the question, is not used. Same in every arm. |
| contradictory_drafts | pass | abstains correctly with irrelevant spans. Added arm: the stating span ranked 5 of 8 and was not supplied to top 1 or 2; with all 8 it was lost again. |
| unavailable_fact | **pass** | top 1 and top 2 abstain. **All five spans: "Mr. Bennet's first name is Bennet"**, the cycle 2 fabrication back, from "Mr. Bennet was among the earliest of those who waited on Mr. Bingley". The regex missed top 2's abstention wording; by hand it is an abstention. |
| creative_request | pass | marked new writing; with the entail span supplied the draft follows the source, with one garbled clause. |

### Reading

1. **Fewer spans is safer, measured twice now.** Five spans brought the
   fabrication back on the unavailable fact; one span did not. One span
   recovered the fortunes answer correctly for its input; two and five
   spans stitched false relations. Top 1 never fabricated and never stitched.
2. **The embedder is a usable first-stage specialist for long stating
   spans and not for short ones**, on this instruction. 4 of 5 first places
   in the realistic pools; every miss is a span under 50 characters or one
   that does not share the question's wording. A question-shaped instruction
   or a cross-encoder is the next thing to measure for the ranker, not the
   composer.
3. **The composer's remaining failures are inference, not reading.** Across
   runs 3 and 4 it quoted the right sentence every time it was supplied
   alone and failed only where the answer required one step the sentence
   does not state: the eldest Lucas child is Charlotte; Lydia is the
   youngest. The entail gloss is muddled in both arms even with the right
   sentence. This is the ceiling of the 1.7B composer on these cases, and it
   is a different problem from the structural ones this cycle fixed.
4. **Harness:** the abstention regex missed one true abstention; mechanical
   passes remain a screen, hand review the verdict. The sentence cut did no
   work and is kept because it is a correctness bound, not a heuristic.

Decision for the production design, from the four runs together: byte-verified
spans only; a deterministic ranker before the composer; one span, or one per
asked-for value, not a packet; abstain in software when nothing verifies.
The ranker's instruction and the two-value case are the next measurements.
Nothing in production changed in this cycle.

Addendum (REDTEAM-020 R2): the one extraction call that hit its 256-token cap
was recorded as status `none` (model said the unit does not answer). It was an
unparseable reply, not an answer. `build/ux4/record-rescored.json` re-derives
every status from the saved responses under the corrected rule: one call moves
from `none` to `unparsed`; no other status, rank or composition figure changes
(answers 57 of 58, expected-true 9 of 9, expected-false verified 20).
