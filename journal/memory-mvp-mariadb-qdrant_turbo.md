# Memory MVP — adversarial verification and refinements

## TEST -> MODEL -> REFINE

**Historical failure:** the upstream report's issue #5796 documents small model
requests coexisting with costly host-history handling. Source inspection also
finds all-history reads/copies/cache work in Scribodo. Therefore prompt compression
alone is rejected as a sufficient P0 solution; paging belongs in the MVP.

**Protocol counterexamples:**

| Weaker candidate | Failure witness | Refined design |
|---|---|---|
| Trust a returned Qdrant point as current text | Enqueue v1; SQL changes to v2; delayed v1 write finishes | SQL rehydration/version/status check; immutable-version point IDs |
| Let vector metadata authorize scope | Wrong-project hit, revoked scope or remote request | SQL scope admission and local-only memory path |
| Use vector payloads when the DB is down | Index remains available after SQL outage | Fail closed for memory authority; retain document-edit availability |
| Advance coverage to the highest finished packet | Packet 2 finishes before packet 1 | Separate contiguous ingestion and coverage frontiers |
| A worker may publish because it once held a lease | Lease reclaimed before old completion | Monotonic fence plus source/policy-epoch checks in the publish transaction |
| Summary rows are a safe materialized cache indefinitely | Source/policy changes while model works | Immutable provenance, epoch invalidation and stale-result rejection |

The naive "SQL summary + nearest Qdrant text" proposal fails these cases. The
refinement removes a distributed-transaction requirement: SQL commits durable
intent, the vector writer retries idempotently, and reads always validate SQL.

## Formal model

`docs/models/memory_mvp_model.py` explores two source/policy/lease versions, two
project identities, SQL up/down, local/remote requests, delayed index delivery,
captured workers, and three work packets. Each candidate-gate witness holds the
other candidate gates valid, avoiding a false conclusion from confounded errors.

Executed result in this pass:

- 1,941 reachable states through depth 8; 1,536 captured-job checks.
- Seven isolated candidate/publication counterexample categories.
- A maximum-completed watermark counterexample; all 18 refined contiguous-prefix
  transitions pass.
- All 36 synthetic budget packing/refusal cases pass.

These results apply to the finite model, not the production implementation. It
does not execute databases, model inference, network failures or real tokenizers.

## Additional design attacks

- **Summary misses the only clue:** keep raw-unit retrieval independent of brief
  wording; measure misses and abstention. No perfect-recall claim is established.
- **T2 tree grows forever:** keep only selected checkpoint/brief metadata in the
  working plan; store cold parents/children on disk, with maximum fan-in and no T3.
- **One source is huge:** cap byte windows as well as rows/tokens, including GUI
  display and ingestion. SQL LIMIT alone is not a work bound.
- **Qdrant replays stale/deleted data after cleanup:** source eligibility changes
  first in SQL. Cleanup delay affects storage/recall completeness, not authority.
- **Document detached:** remove document retrieval; do not pretend to erase text
  already present in the conversation. Do not invent old revision snapshots from
  current `message_documents` rows.
- **Revocation races dispatch:** define a request-admission/as-of point and
  best-effort cancellation after it, rather than promise retraction of sent data.
- **Vector/summary quality on a 1.7B model fails:** compare matched simpler arms;
  do not solve it with an unauthorized larger model or remote fallback.

After the refinements, a second protocol pass found no additional counterexample
within this finite scope. Real integration, performance, quality, indexing
generation, restore/fork and sustained-use verification remain explicit gates.
