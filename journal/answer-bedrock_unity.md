# Cross-system contract audit

The target is one coherent memory transaction, not independent successful
database calls:

`author question -> query representation -> vector/lexical candidates -> SQL
source binding -> evidence selection -> context composition -> generated claims`

## Code observations, distinct from causal conclusions

1. `src/llm/semantic.c:156-158` instructs the query embedder to take a **statement**
   and retrieve established facts about the **same people, places or things**.
   `memory_vectors_search_with_documents` reuses that instruction for the author's
   question. This is a topical-retrieval objective, not explicitly a
   question-answering evidence objective. It is a concrete interface mismatch;
   its contribution to missed sources must be measured, not assumed.
2. SQL hydration checks database/project/conversation ownership, consent,
   generation, revision and exact source bytes. Those are source-integrity
   guarantees, not guarantees that a passage answers the question or supports a
   particular subject/relation/value assertion.
3. Fusion in `context_planner.c` combines lexical/vector rank and fits whole
   units into a budget. It has no answerability or subject/relation check.
4. The request combines retained conversation, document heads and selected
   sources. The income comparisons independently show failed answers after
   adding the selected packet and after adding prior transcript. The sources
   are not the only input influencing inference.
5. The model response is persisted without a source-bound claim validation step.
   Raw-response tests already contain the misattribution, excluding UI display
   as a necessary cause for those observations.

These observations do not imply SQL should decide literary meaning, vectors
should become canonical facts, or an LLM proposal should become author canon.
They identify where each system's guarantee stops, and what the next system
currently assumes beyond that guarantee.

## Paired retrieval-intent measurement

`tools/answer_unity_query_probe.py` uses one real SQL corpus, one indexed vector
set, the original 41 questions and unchanged answer keys. It compares the
existing topical instruction with fiction-question answering and the standard
search-query instruction. Arm order is rotated by question. Eligibility,
document embeddings, lexical retrieval, fusion, hydration, admission and limits
are unchanged. Only query embeddings vary. All generated sources and binaries
are diagnostic-only and frozen locally.

The existing fingerprint is deliberately retained for the unchanged index in
this comparison. This is **not** a safe production migration or a claim that
query-purpose versioning can be ignored. A promoted change would need an
explicit query-purpose contract and compatibility tests without changing the
observer's retrieval behavior.

Registered targets: all 14 exact document keys admitted; all 24 measured chat
tasks fully sourced; zero source-range/reference-budget violations. Report
discovery and admission separately. Better reach, if observed, still does not
establish faithful answers.

### Result: query-instruction change rejected

All three arms completed all 41 questions on the same corpus/index with all
harness gates passing and zero source-range/reference-budget violations.

| Query intent | Exact document keys discovered | Exact document keys admitted | Chat tasks fully sourced |
|---|---:|---:|---:|
| Existing topical instruction | 13/14 | 10/14 | 24/24 |
| Fiction-question answering | 13/14 | 9/14 | 24/24 |
| Standard search-query instruction | 13/14 | 9/14 | 24/24 |

Evidence: `build/answer-unity-query/`. The code-level intent mismatch is not
established as the cause of these misses. Neither alternative is promoted.

## Reproduced integration defect: scope is applied after top-k

The host computes the cutoff excluding history already retained in the actual
LLM request. Previously that cutoff was applied only in `unit_valid`, after
SQL and vector search had already returned their top 16 candidates each.
Neither candidate API accepted the cutoff. The Qdrant payload did not carry
chat sequence, so its server-side filter could not enforce that scope.

This violates the shared retrieval contract: already-present context competes
for the finite discovery budget, then is discarded, potentially leaving no
archive evidence even though the exact older source is indexed and eligible.

The regression uses real SQL and Qdrant with deterministic fake embeddings,
not real-model quality inference: one older 400-byte source and 32 recent
near-duplicate query units. All **33 units** are indexed. With cutoff 1:

- Before: both discovery calls succeed; **32 candidates, zero admitted units**.
- Positive control: querying the old source's distinguishing term retrieves it.
- Before suite: **681 checks, one assertion failure**, no crash or timeout.
- After shared cutoff: **two candidates (one from each channel), one admitted
  original**; **681 checks, zero failures**.

Evidence: `build/answer-starvation-{before,after}.log`, failing source/executable
snapshot under `build/answer-starvation-control/`. This is a reproduced cause
of archive starvation, not proof that it explains every novel-answer failure.

The correction carries the actual retained-context cutoff through production
SQL and vector candidate callbacks. SQL constrains sequence before ORDER/LIMIT;
Qdrant applies the same range in its chat branch before top-k. Saved revision
eligibility is independent of chat sequence. SQL hydration and final cutoff
checks remain authoritative against untrusted vector payloads.

Chat sequence is added to the vector projection, with a changed embedding
protocol fingerprint. Old-format collections are therefore not silently mixed
with new searches: the existing missing-collection worker path reseeds SQL
generation before rebuilding the projection. Original sources and consent are
unchanged. Legacy unbounded APIs remain available to their existing callers.

Targeted vector protocol/allocation checks passed **16,658 checks**; planner
and planner allocation checks passed. Additional direct per-channel cutoff
assertions and broader validation follow separately. Model-answer acceptance
has not been rerun for this correction.

Commit checkpoint: the follow-up serial run has passed `all`, `test`, `dbtest`
and `sanitize`, including the added direct per-channel cutoff assertions.
`tsan`, `analyzetest`, `analyze` and `uitest-db` are not yet certified at this
checkpoint. Evidence remains under `build/answer-cutoff-validation/`.
`tools/test_memory_cutoff_mutations.py` has been added for independent SQL/vector
guard controls but has not yet been run; do not count it as passing evidence.

Post-commit result: all eight targets completed with exit code zero: `all`,
`test`, `dbtest`, `sanitize`, `tsan`, `analyzetest`, `analyze`, `uitest-db`.
Evidence: `build/answer-cutoff-validation/results.json`. This validates the
mechanics at `1bfd022`, not real-model answer correctness. A subsequent
red-team pass is tracked separately from these earlier green results.
