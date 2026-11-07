# Memory MVP — synthesis

**Implementation guide:** [MEMORY-002 — MariaDB and Qdrant memory MVP](../docs/MEMORY-002-mvp-design.md).

## Core decisions

1. MariaDB is authoritative; Qdrant returns candidate locators only. Publication
   uses durable SQL index intent, idempotent delivery and fenced acknowledgments.
2. The first path is local/current-conversation memory, with saved explicitly
   attached documents available for retrieval. Summaries cover chat text, not
   undisclosed findings or implicit project-wide canon.
3. Start with independent T1 segment briefs and exact-source recall. Add one
   bounded T2 checkpoint mechanism as a comparison arm, not a general tier engine.
4. Preserve originals and child provenance. Only selected memory enters a bounded
   context; no all-history copy is an acceptable prerequisite.
5. Page history/caches first, then implement ingestion/indexing/retrieval, T1,
   optional T2, and the visible writing/recovery loop.

## Verification and acceptance

The finite protocol model exercised 1,941 reachable states, 1,536 captured-job
checks, 18 contiguous-prefix transitions and 36 synthetic budget cases. It rejects
tempting weaker protocols; it is not an application/database/model-quality proof.
See the [verification record](memory-mvp-mariadb-qdrant_turbo.md).

An implementer must then pass real MariaDB/Qdrant fault/retry/isolation tests,
matched real-model recall/fidelity comparisons, bounded resource instrumentation,
sanitizers, UI/recovery/offline acceptance and an hours-long sustained-use run.
Choose the simplest useful arm. Preserve the pinned model, author authority and
remote boundary even if a more complex memory arm fails.

## Decisions still requiring agreement

Approve numerical performance/quality gates, the held-out evaluation corpus and
the first runtime implementation slice. This pass changes planning documents and
adds a standalone abstract-model checker only; it does not migrate data, install
dependencies, alter inference behavior, commit or push.
