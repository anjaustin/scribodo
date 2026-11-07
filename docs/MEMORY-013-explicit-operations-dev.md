# Explicit Recall/Draft operations: mixed gains, screen failed

**2026-11-07. Frozen native DEV experiment, not held-out acceptance.**

## Outcome

Explicit specialization improves some author-choice recall, but the tested
instruction-only separation does not reliably enforce the Recall/Draft boundary.
The frozen screen required all 20 specialized conditions to pass; they did not.
**Do not implement the production operation prototype from this result.**
Production routing, model, prompts, memory tiers, consent and defaults are
unchanged. Do not revise these prompts until this fixture turns green. The
earlier V2/native failures and T1/T2/default-on blocks remain in force.

## Experiment

The author explicitly authorized testing Recall and Draft as separate jobs.
`tests/fixtures/memory_native/operations-registration.json` freezes both operation
instructions, budgets, review rules and the stop condition before inference.

- Ten tasks: seven recall, including an accepted/unaccepted choice counterfactual;
  three draft tasks covering current style override, unspecified coat color and
  protected fire ambiguity.
- Two layouts: alternating recent history and repeated consecutive author turns.
- Two arms: production general creative persona and specialized operation prompt.
- Both arms use episode presentation, identical original SQL sources within each
  cohort and the same current question. The authored fixture explicitly selects
  the operation; no model or classifier routes it.
- Same pinned local BF16 model, effective 8,192-token context, 1,024 reply tokens,
  temperature zero, repetition penalty 1.05, cache enabled, 500 ms memory budget,
  existing candidate/reference/receipt bounds, and no T1/T2.
- Native SQL hydration, final admission, fitting, streaming and postprocessing.
  Candidate discovery is still the frozen eligible SQL episode seam; this does
  not measure Qdrant discovery or whole-manuscript coverage.
- Arm order is counterbalanced by conversation serial; generated replies are
  not saved as future source history. This is not a controlled latency study.

`tools/memory_native_eval.c` changes only its own diagnostic translation unit's
persona pointer between joined workers. There is no new application API or
production operation setting. `tools/memory_operation_eval.py` freezes executable,
input, registration and source hashes before running the disposable database.

## Mechanics pass; semantic screen fails

Run `build/mo-nvvlimc8` completed all 40 answers in about 306 seconds. There were
no errors or truncations. All answers had Sources; every arm pair admitted the
same three original IDs; current requests and frozen inputs remained intact.
Production source hashes still match the frozen manifest. Automatic report:
`SEMANTIC_REVIEW_REQUIRED`, with `quality_pass=false`.

All 40 complete visible responses were reviewed by the assistant, not an
independent human. `journal/memory_operation_review.json` records every verdict,
literal evidence quote, uncertainty and source-artifact hash.

### Same author-task clauses in both arms

These are strict small DEV judgments, not population accuracy estimates:

| Operation / history | Generalist pass / fail / uncertain | Specialized pass / fail / uncertain |
|---|---:|---:|
| Recall / alternating (7) | 2 / 4 / 1 | 4 / 3 / 0 |
| Recall / consecutive user (7) | 5 / 2 / 0 | 4 / 2 / 1 |
| Draft / alternating (3) | 2 / 1 / 0 | 2 / 1 / 0 |
| Draft / consecutive user (3) | 1 / 2 / 0 | 1 / 0 / 2 |

Specialized core results total 11 pass, 6 fail, 3 uncertain. The specialized Draft
instruction additionally requires draft-only output. Two otherwise adequate
two-sentence drafts include unrequested source recaps/notes. Charging those
extra operation-contract violations yields **9 pass, 8 fail, 3 uncertain** across
the 20 specialized conditions. Generalist commentary is not silently judged by
a draft-only system instruction it did not receive; the common-task table above
keeps that distinction explicit.

Uncertain cases block advancement but are not presented as definite violations.
None of the judgment ambiguities can change the no-go: multiple clear failures
remain in Recall and Draft independently.

## What improved

- Specialized Recall correctly distinguishes both accepted and unaccepted cobalt
  choices in both layouts, with correct author attribution. It does not merely
  answer “not decided” for every proposal question.
- It reports the night-train choice and abandoned ferry without the generalist's
  ambiguous request to revisit disposition of the discarded alternative.
- Specialized Draft produces exactly two warm mentor-thank-you sentences under
  consecutive history, where the generalist produces five mixed sentences.

## What still fails

- **Resume state:** the specialized repetitive-history recap both quotes the
  correct evidence and calls the tower/bell constraint “still pending.” A later
  correct bullet list does not erase that earlier change of meaning.
- **Source/authority fidelity:** specialized alternating fire Recall treats an
  earlier assistant reveal suggestion as a later decision and fails to recognize
  that S1 explicitly presents accounts of the fire.
- **Incomplete evidence:** specialized astrolabe Recall substitutes a local “No”
  for the manuscript-wide cannot-determine answer. The repetitive variant also
  invents S1–S64 and misdescribes the three originals as weather-only material.
  The review does not exaggerate a locally scoped No into a claim that a full
  manuscript search actually occurred.
- **Task length:** specialized alternating mentor Draft produces two whole
  multi-sentence letters instead of two sentences.
- **Boundary discipline:** specialized Draft can still add source recaps and notes
  despite the explicit draft-only instruction. Hedged cobalt hints and a fire
  passage's unqualified slipped-lamp mechanism are recorded as uncertain
  constraint preservation, not overstated as definite canon changes.

## Interpretation

The division of responsibilities remains sound, but assigning roles by prompt
does not itself enforce the division. Both operations still ask the same
unqualified model to interpret and preserve author state. New labels for the
jobs do not create a reliable semantic boundary.

A future design decision should distinguish **mechanically enforceable work**
(scope, exact original display, immutable provenance, explicit author acceptance)
from **fallible semantic work** (interpreting commitments and preserving them in
arbitrary prose). Host code cannot generally verify the latter. A narrower
author-controlled evidence/creation workflow or a separately qualified model
remains a legitimate direction; neither is implemented or qualified here.

Next is a product/capability decision, not another summary tier or a prompt
rewrite against these same cases. Fully automatic intent continuity with the
tested configuration remains unproven and fails this screen.

References: [causal diagnosis](MEMORY-012-causal-continuity.md),
[frozen protocol notes](../journal/memory_operation_experiment.md),
[complete review](../journal/memory_operation_review.json).
