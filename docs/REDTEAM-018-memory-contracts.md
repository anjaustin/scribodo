# REDTEAM-018 — Memory remediation contracts and failure paths

**Date:** 2026-11-09. **Baseline:** `1bfd022`; validation record committed as
`66cf105` before this pass. **Status:** remediation and final verification complete;
no unresolved confirmed findings within this review's scope.

## Scope and acceptance

Assistant-led review of the memory remediation, not an independent security
audit. CI configuration is unchanged. The review covers request construction,
retained-history boundaries, SQL/vector candidate scope, projection versioning,
allocation failures, and the integrity of the new diagnostic tools.

Acceptance for this pass requires: reproduced failures fixed; unchanged controls
passing; weakened guards detected by normal assertion failures rather than
build errors/crashes/timeouts; complete local validation; and no source drift
during the final run. This is a bounded engineering gate, not proof that the
whole product or the pinned model is flawless. The previously measured real-model
answer failures remain a separate open acceptance gate.

## Findings and remediations

### R1 — Failed assistant-history preprocessing could re-send reasoning

The original allocation sweep covered the current author turn and an edit
passage, but not assistant-history cleanup. A three-message history containing
an earlier reasoning block reproduced **three non-control requests**, including
**two containing the private reasoning marker**, under injected allocations.
The builder's fallbacks to original content bypassed failed cleanup; failed
speaker-label stripping could also publish a different history representation.

The builder now rejects failed reasoning separation or speaker-label stripping.
It never substitutes the original assistant content for failed cleanup. The
regression permits only a complete control-equivalent payload or NULL, and
independently rejects the reasoning marker in any published payload.

An initially proposed splitter rewrite was **removed** during verification:
the existing append helper frees its buffer and returns NULL on failed growth.
A mutant restoring the original splitter passed all new guards, disproving the
claim that a separate partial-buffer fix was necessary. The required correction
is at the caller's fallback boundary. The failed mutation attempt is retained
under `build/redteam-memory-final/` and `build/redteam-memory-mutations/`; it is
not counted as a detected mutant.

### R2 — Sampling updates were not all-or-nothing

`chat_apply_sampling` deleted existing settings, ignored failed JSON additions,
then serialized the reduced object. The pre-fix sweep measured **12 partial
requests for chat and 12 for edits**, at 29 allocation positions per control,
tested with one-shot and persistent failures.

Every required replacement must now succeed before publication. Failed parsing,
non-object input, a missing settings pointer, or a failed field addition returns
NULL. The caller already refuses a NULL request before inference. The tests
also preserve an edit's larger requested output allowance.

The combined pre-fix persistence-fault suite recorded **1,920 checks / 29
failures**. Failing sources and executables are preserved under
`build/redteam-memory-before/control/`, with the original logs alongside them.

### R3 — Invalid sequence projections reached Qdrant

The new cutoff search rejected malformed projected sequences, but upsert's
unit-validation boundary accepted negative or oversized chat sequences. Tests
using **-1, INT_MAX+1 and INT64_MAX** reproduced successful invalid upserts and
backend requests. The vector protocol suite failed **nine assertions in 143
checks**. Such a projection could subsequently make an otherwise successful
search fail validation.

Chat sequence is now validated in `memory_vectors_unit_valid`, before any
backend operation. All three invalid values must fail with a reason and zero
backend requests. Boundary tests additionally require INT_MAX to be indexable,
excluded at cutoff INT_MAX, and included at the INT64_MAX all-history sentinel.
Document projections remain independent of the chat cutoff.

### R4 — An errored leading user could leave an orphan assistant prefix

The public selected-history builder could align to an errored user row, skip
that row during serialization, and then start history with an assistant reply.
The worker's bounded-history copier normally removes errors first, but the
public builder's own boundary contract should not depend on that assumption.

Prefix alignment now skips errors as well as non-user rows. A regression requires
the exact retained index and a request beginning at the next valid author turn.
The mutation control restores the former alignment rule to test detection.

### R5 — A diagnostic oracle changed source text without its binding

One evidence-probe arm replaced a later-scene source body with the opening
passage but retained the later scene's document/revision metadata. Earlier
notes already excluded that output from provenance-valid acceptance; the tool
itself still allowed future repetitions of the invalid comparison.

Both oracle phases now move the entire captured opening source binding with
its text. No hard-coded 1,856-byte slice from a mutable manuscript is used.
The test checks the expected document/revision binding and rejects the stale
later-scene binding.

### R6 — Diagnostic questions were not part of the frozen capture

The probe read the question from the mutable latest answer-run export, not its
capture directory. A later export could therefore alter the comparison while
the captured request stayed fixed. Capture now freezes the transcript, and the
probe reads and hashes only those frozen question inputs. Oracle input requests
are also copied into each output inventory.

The replay build explicitly logs that generation is replaced with frozen replies
and that UI answer assertions are not inference evidence. It verifies the live
author question against the frozen reply's question and refuses remote replay.
It supports a fresh output path without overwriting old evidence.

R5/R6 controls ran the new tests against the saved old Python implementation:
**two tests failed with zero test errors**. The corrected seven-test diagnostic
suite passed, including one-factor comparisons and unchanged source inputs.
Evidence: `build/redteam-probes/`.

### R7 — Probe temporary resources were machine-specific

The probe baked this developer's `/var/folders/...` path into the tool and could
leave a temporary directory behind on startup failures. It now uses a private
machine-local temporary directory, validates the Unix-socket path length and
owns cleanup through a context manager. Server termination is verified on
request failure, including the timeout-to-kill path. CLI tests reject invalid
case indices and generation limits before creating output directories or
starting a model.

## Iteration and controls

- Initial targeted corrections passed the persistence, vector, vector-allocation
  and LLM suites; final counts follow after the later boundary/property additions.
- The SQL/vector cutoff controls completed independently: unchanged **685 checks,
  zero failures**; bypassing SQL caused **two** assertion failures, bypassing
  vector filtering caused **four**. Both mutants finished normally. Evidence:
  `build/answer-cutoff-mutations/`.
- `tools/test_memory_redteam_mutations.py` tests unchanged controls and the
  sampling-publication, history-fallback, sequence-validation and errored-prefix
  guards. `tools/test_product_memory_mutations.py` and
  `tools/test_memory_cutoff_mutations.py` are rerun separately.
- Mutation tools accept `--output` so later rounds preserve, rather than replace,
  earlier evidence.
- A deterministic 1,000-input text corpus checks that visible-only reasoning
  separation matches separation with both outputs. Existing text rules are not
  rewritten to make a test pass.

## Final verification

`build/redteam-memory-final-v2/` completed **all 15 gates with exit code zero**:

- Three mutation suites: **six unchanged control runs passed; all ten mutants
  were detected by normal assertion failures**. No compiler error, crash or
  timeout was counted as a detection.
- Twelve local targets: `all`, `test`, `dbtest`, `sanitize`, `tsan`, `analyzetest`,
  `analyze`, `uitest`, `uitest-db`, `offlinetest`, `llmtest`, `uitest-real`.
- Static analysis reported **zero warnings across 83 source files**.
- The source-hash comparison completed with **zero changed files**. The
  unsuccessful retrieval experiment flags were unset throughout verification.

Final focused controls passed **1,922 persistence-fault checks**, including
zero partial assistant-history requests, zero reasoning-marker leaks and zero
partial sampling requests. Vector protocol controls passed **147 checks**,
including all invalid sequence cases and boundary sentinels. Headless LLM
controls passed **4,690 checks**, including the added text-equivalence corpus
and failed-prefix boundary.

| Weakened guard | Normal assertion failures |
|---|---:|
| Sampling publication | 24 |
| Assistant-history fallback | 5 |
| Invalid sequence rejection | 9 |
| Failed-user prefix alignment | 2 |
| Copied-tail exclusion restored | 1 |
| Recall reservation removed | 2 |
| Chat publication | 90 |
| Edit publication | 34 |
| SQL pre-top-k cutoff | 2 |
| Vector pre-top-k cutoff | 4 |

Evidence: `build/redteam-memory-mutations-v2/results.json`,
`build/redteam-product-mutations-final/results.json`,
`build/redteam-cutoff-mutations-final/results.json`, and the final run's separate
target logs and source inventory. These failure counts describe the injected
controls, not the frequency of real-world failures.

The corrected capture application also compiled. A separate native replay
smoke test passed **three checks**: a matching question replays the exact frozen
reply; a mismatched question refuses; a missing fixture refuses. It made no
generation request. Evidence: `build/redteam-capture-check/`. This is diagnostic
plumbing verification, not a model-quality result.

The final review found no additional unresolved defects in the declared scope.
The deliberately unsuccessful splitter-mutation round remains preserved rather
than being relabelled as a pass. No claim of exhaustive verification is made.

## Remaining limits

No new model, prompt or retrieval policy is promoted. Failed excerpt/small-unit
experiments remain default-off. The actual sources still require SQL admission
after vector discovery, and source provenance is not a proof of semantic answer
correctness. Real-manuscript hallucination, incorrect attribution, irrelevant
continuation and absence handling measured in MEMORY-022 are not erased by
green mechanical tests. Distribution, accessibility, sustained author use and
other open PRODUCT-GAPS items are outside this bounded review.
