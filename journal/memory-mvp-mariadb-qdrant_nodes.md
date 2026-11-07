# Memory MVP — nodes and dependencies

## Observations and decision points

1. **Archive and working context differ.** Large durable history need not become
   a large inference request or in-memory message array.
2. **MariaDB owns validity.** Text, scope, coverage, versions and job state must
   remain authoritative even when vectors are stale or absent.
3. **There is no shared SQL/Qdrant transaction.** A crash can occur between any
   successful writes; indexing therefore needs retryable durable intent.
4. **Summaries are lossy derivatives.** Coverage is an address map, not evidence
   that every fact survived summarization. Exact retrieval must remain independent.
5. **The first hierarchy should be small.** Independent segment briefs and one
   checkpoint level can test the upstream mechanism without a general tier engine.
6. **Sources have different permissions.** Current-chat text, attached manuscript
   revisions, undisclosed findings and remote-provider history are not interchangeable.
7. **Background completion order is not source order.** A maximum completed ID is
   not a safe ingestion/coverage watermark.
8. **A lease is not a permission to publish forever.** Retries and source changes
   need generation/fence checks, not just worker timeouts.
9. **Recall and liveness compete.** More candidates, summaries and tool rounds may
   improve some answers while destroying the responsiveness goal.
10. **Host scaling is part of the MVP.** Existing full conversation reads/copies
    and message layout caches cannot be left outside the scope of P0.
11. **The local model is an evaluation dependency.** Structural JSON/source checks
    do not establish semantic fidelity or autonomous tool competence.
12. **Failure must be visible without stopping prose editing.** Derived-index
    outage, missing briefs and store outage have distinct behaviors.

## Tensions and dependencies

- Nodes 2/3: durable authority versus two-service updates; publication and indexing
  must not pretend to be atomic across databases.
- Nodes 4/5: useful compression versus irreversible detail loss; retaining sources
  and limiting hierarchy are separate requirements.
- Nodes 6/9: broad recall versus predictable privacy and bounded work.
- Nodes 7/8: concurrency versus contiguous progress and stale-worker exclusion.
- Nodes 1/10: reducing model tokens does not remove all-history host costs.

## Options to resolve

Compare a proxy, kernel sidecar and native planner; compare retrieval-only,
segment briefs and two-level briefs under identical models/budgets. The problem
has genuine consistency, scope and quality tensions, so a full reflection and
adversarial verification pass is warranted.
