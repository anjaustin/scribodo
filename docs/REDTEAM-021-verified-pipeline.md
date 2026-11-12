# REDTEAM-021: adversarial review of the verified-answer pipeline C

**Date:** 2026-11-10
**Reviewed tree:** commit `4840d80`: `src/llm/rerank.c/.h`, `src/llm/answer_pipeline.c/.h`,
the pipeline branch and builder refactor in `src/llm/chat.c`, the third
server configuration in `src/llm/server.c`, the wiring in `src/main.c`,
`tests/test_answer_pipeline.c`, and the two Python tools added with them.

## Method

Every new line was read against three questions: what does a server reply,
a unit's text or the author's words make this code do that it should not;
what happens on the worker thread when a deadline, a cancel or an allocation
fails; and which number in the journals could be wrong without a test
noticing. Suspicions were measured (struct sizes, clocks, the live traces of
`pipe-new2`) before being written down. Each correction has a mutation
control compiled from a generated copy of the source with the project's own
compiler command and linked against the unchanged objects
(`build/redteam021-mutations/`); the control must fail the test the
correction passes. The pipeline test was added to the ThreadSanitizer list.
`make test`, `sanitize`, `tsan` and `analyze` were rerun on the remediated
tree.

## Findings and remediations

### R1 — the pipeline's vector search ran on the unbounded client (P2, fixed)

`chat.c` passed `s->memory_vectors` to `answer_pipeline_run`. The plan path
uses `semantic_client_bounded(…, &budget.stop, budget.deadline_ms)` so a
slow or hung Qdrant cannot hold the worker past the preparation budget; the
pipeline had no such bound and would have waited for the HTTP client's own
timeout, outside the session's cancel. Fix: a bounded client created with
the pipeline's deadline and the session's cancel, destroyed after the run.
Measured on `pipe-new2`: the semantic search returned in the hydration phase
on all ten turns, so no run was affected; the hazard was latent.

### R2 — 58 KB of candidate and unit arrays on the worker stack (P3, fixed)

`MemoryUnit` is 592 bytes and `MemoryCandidate` 160; the run function held
64 units and 128 candidates on the stack of the chat worker, a pthread with
the platform default stack, and the sanitizers inflate frames further. Fix:
one heap allocation for the work set, freed on every exit path (the three
early returns and the normal path were each checked). The ASan run of the
remediated tree is the verification.

### R3 — the review tool paired traces by position (P2, fixed before the journal was written)

`tools/answer_pipeline_review.py` matched the trace lines to questions by
order. A turn answered without a pipeline trace (the first fixture turn,
before memory was active) shifted every later row by one, so the first
sheet printed each case's candidate ranks from the next question's trace
(the walk question showed its chosen unit at rank 24). Fix: traces are
consumed in reply order by the chosen unit id; a unit chosen for two turns
no longer makes the match ambiguous. Both cases are tests. The pass 2 table
in the journal was produced after the fix and checked by hand against the
receipts.

### R4 — the reranker's memory figure is a placeholder (P3, registered)

`llm_server_rerank_config` sets `mem_fixed` to 1 GiB without a measurement;
the embedder's figure was measured from llama-server's own breakdown. The
context fit subtracts it when the flag is on. Registered in
`docs/TECHNICAL-DEBT.md` with the measurement to take before any promotion.

### R5 — two null dereferences the analyzer could see and I could not (P3, fixed)

`make analyze` on the remediated tree reported `core.NullDereference` twice in
`answer_extraction_parse`: `cJSON_IsString(content)` followed by
`content->valuestring`. The cJSON predicate returns false for NULL, so the
path is unreachable, but the analyzer cannot prove it and the project's
target is zero warnings. Fix: explicit `content &&` and `s &&` guards. The
analyzer is back to zero across the tree; the M3 control was re-anchored to
the guarded line and still kills.

### Reviewed without change

- **Missing yes/no in the top probabilities counts as zero.** With `n_probs`
  20, one of the two tokens was outside the list on 53 of 89 parity pairs;
  the resulting differences stayed inside the 0.026 maximum. Fails closed
  when both are missing. Noted in MODEL-002.
- **The verified block is neutralized; the receipt is raw.** A span that
  contained a control-token lookalike would be shown to the model altered
  and recorded exactly. The receipt is the provenance. Noted.
- **Hydration runs inside the 500 ms preparation budget.** The pipeline's
  SQL (two candidate searches and up to 64 point lookups) happens before any
  HTTP; if the budget expires mid-way, later lookups fail and the run
  proceeds with fewer units, visible as `hydrated < candidates` in the
  trace. On `pipe-new2` hydrated equalled candidates on all ten turns.
- **Extraction occupies the chat slot before the reply.** One synchronous
  completion on slot 0 with `cache_prompt` off; the reply's own request
  follows on the same slot. Measured cost 1.3 to 5.5 s per turn.
- **Manuscript-only candidates are a pipeline rule, not a retrieval rule.**
  Production retrieval is unchanged; the filter is applied after hydration.
  The first stage can still be crowded by chat units before the filter
  (the "not sixteen" unit was outside 64 candidates on pass 2).
- **ASCII-only lowercasing in `answer_normalize`.** Both sides of every
  comparison go through the same function, so verification is consistent;
  it differs from the Python classifier only for non-ASCII letters. The
  tolerance of the whole normalization was measured on 2026-11-12 over 681
  recorded spans under eighteen mutations (journal/answer-cycle4_tolerance.md):
  every meaning change rejected; the one defect, a marker dropped inside a
  word, closed; a hyphen split and a plural possessive apostrophe lost remain
  accepted as rendering (the words are unchanged and the manuscript's bytes
  are shown).
- **Fixture scorer with the fake backend.** With the flag set and no real
  servers, `rerank_client_create(NULL)` scores by content-word overlap and
  the extraction call is not attempted (no chat server), so the request
  carries "No verified passage"; deterministic, never a quality claim.

## Final verification

On the remediated tree (`build/redteam021-*.log`):

| Target | Result |
|---|---|
| `make test` | 95,674 C checks, 0 failures; 104 Python tests in 19 files, all OK; `test_answer_pipeline` 81 checks |
| `make sanitize` | same counts under AddressSanitizer and UndefinedBehaviorSanitizer, 0 runtime reports (run before R5; R5 changes two conditions only) |
| `make tsan` | `test_answer_pipeline` added to the ThreadSanitizer list; 0 data-race reports |
| `make analyze` | 0 warnings (2 before R5) |
| Mutation controls | M1 sentence cut disabled, M2 emphasis kept, M3 cut reply counted as parsed, M4 missing token scored as certain, M5 unbounded window, M6 composer sentence in the production builder: 6 of 6 killed; control passes (`build/redteam021-mutations/results.json`) |

`git diff --check` clean; `pyflakes` clean over tools and tests. The live
runs `pipe-old`, `pipe-new` and `pipe-new2` predate R1, R2 and R5; none of
the three touches a number in those runs (R1 bounds a search that returned
within budget on every turn, R2 moves memory, R5 guards an unreachable
branch), so the journal's figures stand.
