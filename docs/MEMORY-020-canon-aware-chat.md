# MEMORY-020 — Canon-aware chat (P1): registration, implementation, first DEV run

**2026-11-08. The first P1 experiment under [MEMORY-014](MEMORY-014-canon-first-continuity.md) §5, authorized by the P0 contract in [MEMORY-015](MEMORY-015-effective-memory-contract.md) after S1.**

**Status:** implemented behind a per-session flag, registration frozen
before the first model run, first DEV run RESULT_STATUS. Nothing is on by
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

RESULTS

## Limits

- Development episodes from the S2 fixture; the canon wording was generated
  from the planted turns by category, which is a best case for a committed
  record and says nothing about how authors actually write canon.
- The production presentation's consecutive-layout failure is not expected
  to move and is not what this experiment tests.
- Pass or fail, this is one model, one configuration, one fixture; the
  held-out gate of MEMORY-014 §8.4 is untouched.
