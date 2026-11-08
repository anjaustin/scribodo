# MEMORY-020 — Canon-aware chat (P1): registration, implementation, first DEV run

**2026-11-08. The first P1 experiment under [MEMORY-014](MEMORY-014-canon-first-continuity.md) §5, authorized by the P0 contract in [MEMORY-015](MEMORY-015-effective-memory-contract.md) after S1.**

**Status:** implemented behind a per-session flag, registration frozen
before the first model run, first DEV run complete: **stop condition met**. Canon removed every authority and open-state failure in the alternating layout and introduced over-anchoring in four cases, so P1 does not advance in this form. Nothing is on by
default; no prompt, tier, consent or default changed. A pass here is
development evidence only, never held-out acceptance.

## The question

MEMORY-014 §4.5 argued that the model's hardest job in the failed
experiments was inferring authority: whose decision a statement was, and
whether an ambiguity was pending or protected. Canon converts that inference
into a stated fact the host can hand over exactly. P1 asks whether supplying
the author's committed canon as a separate, exact reference class removes
those failures in chat, without creating a new one: the model writing about
the block, or inventing authority when canon is silent.

## Registration (frozen in `tests/fixtures/memory_native/canon-registration.json`)

- Arms: `canon_off` and `canon_on`; production (legacy) presentation only, so
  the effect is not confounded with episode packing.
- Layouts: alternating and consecutive author turns, as in MEMORY-011.
- Per case, exactly that case's canon is committed before its answers and
  retracted afterwards, so the project's committed set is the case's own.
  Absence cases commit nothing.
- Measures: the frozen rubric axes, plus over-anchoring and invented
  authority when canon is silent.
- Hypothesis: `canon_on` removes the protected-ambiguity and rejected-proposal
  failures in the alternating layout, and does not rescue the consecutive
  layout, because that failure is presentation, not authority.
- Stop: if `canon_on` does not eliminate the authority and protected-ambiguity
  failures where canon states the decision, or introduces over-anchoring or
  invented authority on absence cases, P1 does not advance and the fixture
  is not retuned.

## Implementation

`src/llm/canon_reference.c` builds the block from the current committed
version of every canon entry whose wording shares a term with the question:
committed only, best match first then newest, at most 8 entries and 4,096
bytes, each entry neutralized, with a header that names it as the author's
own decisions and a footer. `canon_reference_apply` places it before the
retrieved evidence in the last user message with a boundary that says what
follows. In the chat worker the whole request is recounted with the real
tokenizer and the block is dropped whole if it would not fit; the author's
request is never clipped. `chat_set_canon_reference` is the opt-in; the
native driver uses it for the `canon_on` arm. The canon entries never enter
evidence ranking; the planner is unchanged.

Tests: `test_recall: canon reference block` (committed only, two-term match
outranks one, bounds, placement, refusal when the last message is not the
author's, nothing supplied when no committed entry matches). Mutation
`canon_committed_only` removes the status filter and is killed.

## First DEV run

Run `build/mn-ld40aqkd`, 2026-11-08 12:49 to 13:18 under `caffeinate -i`,
108 answers, no driver errors, canon block present in every `canon_on`
request whose case has canon and in no `canon_off` request, identical
source IDs across the two arms in all 54 cells. Every answer reviewed under
the frozen rubric; verdicts and rationales in
`journal/memory_canon_review.json` (events hash recorded there).

| Cell | pass | fail | n |
|---|---:|---:|---:|
| canon_off, alternating | 22 | 5 | 27 |
| canon_on, alternating | 19 | 8 | 27 |
| canon_off, consecutive author turns | 2 | 25 | 27 |
| canon_on, consecutive author turns | 2 | 25 | 27 |

What failed, by axis:

| Cell | failures |
|---|---|
| canon_off, alternating | open_state 3 (every protected-ambiguity case treated as pending), author_intent 1 (t23 swaps who proposed and who rejected), citations 1 (t05 cites the filler line as Source 1) |
| canon_on, alternating | over-anchoring 4 (t05, t14, t15, t24), citations 3 (t13, t22, t23), current_task 1 (t25) |
| both arms, consecutive | completion 23 (truncated at the 1,024-token cap), current_task 25 (never addresses the question), factual_fidelity 2 (t18 invents a birthday in both arms) |

**The hypothesis held where it was stated.** With canon present, no
rejected-proposal or protected-ambiguity reply in the alternating layout
misread the authority or the open state: all six read "undecided, rejected
by the author" and "deliberately unresolved" correctly, where `canon_off`
failed four of the six. The implied-decision, superseded and stated-fact
cases were right in both arms.

**The stop condition was also met.** Four `canon_on` replies wrote about
the block instead of answering from it: t05 reproduces the whole block,
boundary text, sources and request verbatim as a "fuller draft"; t14
restates the canon entry as the author's current request and echoes
`[C1 | v1 | committed] ... END OF CANON.`; t15 and t24 return the canon
line in a code block plus the question, with no answer in the model's own
words. The open state survived in all four, so this is over-anchoring, not
invented authority. Three more replies confused the two reference classes
in their citations, calling a canon line "Source 1 (retrieved)" or an
author message "author-committed canon" (t13, t22, t23); the answers were
right, the handles did not bind. `canon_off` had one citation failure.

**Absence cases.** No invented authority in the alternating layout, in
either arm. Note the limit: with nothing committed for these cases no block
is built, so the two arms sent identical requests and this measures the
record's silence, not the model's handling of a present but irrelevant
block. In the consecutive layout t18 invented a birthday in both arms, which
is unrelated to canon.

**The consecutive layout was not rescued**, as registered. The failure is
the presentation: 63 identical filler author turns with no assistant turn
between them, followed by the question. At temperature 0 the model loops
about the repeated line until the reply cap (23 of 54 truncated, 37 to 39 s
each) or asks for "the real text" (27 of 54); only the four absence replies
pass. The same cases under the episode-packed presentation passed 23 of 24
in this layout (MEMORY-017), so the loop belongs to the layout, not to the
facts. The identical filler is a harness choice and sharpens the effect;
the alternating layout with the same filler never truncated.

**Reading.** Canon does the job it was built for, converting the authority
inference into a stated fact the model reads correctly, and the cost is
that the block's framing text is itself quotable and this model quotes it.
Per the registration, P1 does not advance in this form and the fixture is
not retuned. The remaining question is structural, not a prompt tweak:
whether a presentation of canon that carries no header or footer text the
model can echo, or one that enters as a distinct message role rather than
inside the author's turn, removes the over-anchoring while keeping the
authority effect. That is a new registration, and it is only worth running
once the presentation failure of the consecutive layout has a production
answer (the episode decision in MEMORY-017).

## Limits

- Development episodes from the S2 fixture; the canon wording was generated
  from the planted turns by category, which is a best case for a committed
  record and says nothing about how authors actually write canon.
- The production presentation's consecutive-layout failure is not expected
  to move and is not what this experiment tests.
- Pass or fail, this is one model, one configuration, one fixture; the
  held-out gate of MEMORY-014 §8.4 is untouched.
