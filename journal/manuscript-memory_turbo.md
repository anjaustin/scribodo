# Manuscript memory — adversarial design checks

These are design scenarios and inspection results, not executed memory-system
tests. No new memory implementation exists yet.

| Challenge | Result / required invariant |
|---|---|
| A one-line exact fact disappears in a summary | Summary-only proposal rejected; retain raw text and retrieve it by source/lexical/semantic lookup |
| A model receives a small prompt but the app copies 100k messages | Proxy-only solution rejected as sufficient for P0; page history and cap working caches |
| A local summary is reused after selecting a remote provider | Deny unless every underlying source was eligible for that request; local-only memory is not implicitly shared |
| An observer summary mentions an undisclosed finding | Exclude it from creative memory; Discuss remains the only finding-to-chat route |
| A document changes while its summary is being generated | Bind jobs to immutable revision/hash; publish only against matching eligibility/version state |
| Retrieval needs more text than fits | Bound candidates, bytes, rounds and time; return partial coverage/ask to narrow rather than loop or overflow |
| The local model emits an invalid or misleading summary | Validate structure/source references, retain originals, and test semantic fidelity; structural validity is not truth |
| A broad whole-book task needs exhaustive coverage | Use a bounded multi-pass plan with coverage reporting, not a top-k result presented as exhaustive |
| Summaries accumulate indefinitely | Enforce a selected-summary budget; do not put the complete summary tree in each request |

## Model

`eligible sources -> bounded candidates -> selected briefs + exact excerpts ->
final token-budget check -> inference`

Raw sources remain authoritative. Summaries/indexes are versioned derivatives.
The model budget includes instructions, user request, protected edit passage,
recent turns, briefs, evidence, any tools, output reserve and safety margin.

The refinement removes the need for an always-on generic proxy and for autonomous
memory tools in the first experiment. It does not claim all failure modes are
exhaustively closed; quality, budgets and permission UX remain evaluation gates.
