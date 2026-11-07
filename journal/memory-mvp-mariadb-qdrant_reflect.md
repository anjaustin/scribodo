# Memory MVP — decision rationale

**Core insight:** Make memory a versioned, source-addressable projection of
MariaDB, with Qdrant proposing candidates and a bounded native planner deciding
what a specific authorized request may actually read.

## Resolved tensions

- **Two stores versus one authority:** commit source/brief changes and durable
  index intent in MariaDB together. Deliver immutable-version points to Qdrant
  asynchronously; validate every candidate against SQL before using it. Do not
  hold SQL transactions across inference or vector calls.
- **Continuity versus exactness:** independent T1 segment briefs plus an optional,
  gate-controlled T2 checkpoint offer continuity; original chunks remain
  separately searchable/readable. A checkpoint replaces selected prompt
  representation, never source records. Keep only selected frontier blocks in RAM.
- **Privacy versus scope complexity:** the first memory-enabled path is local,
  current-conversation text plus explicitly attached saved document versions.
  Summaries cover chat text only. Remote requests keep their existing behavior
  without this new memory path; undisclosed findings are excluded entirely.
- **Concurrency versus correctness:** publish briefs only against the captured
  history epoch and current fenced job claim; advance only contiguous completed
  work. A late vector write cannot authorize stale content.

## Challenged assumptions

The Qdrant payload is not an authoritative source excerpt. A lease timeout does
not prevent the expired worker from finishing. A completed high-sequence job does
not prove earlier work succeeded. A finite active prompt does not bound SQL result
sets, GUI caches or repeated serialization. These assumptions change the MVP from
"summaries plus vector search" into a small, explicit consistency protocol.

Use native interfaces and the existing local model first, not an always-on
general proxy or model-driven agent loop. Start with retrieval-only and T1
comparisons; retain a concrete T2 design but enable it only if the hierarchy arm
earns its cost. Numeric budgets are proposed configuration values, not measured
performance claims. The following verification record must challenge these
decisions before the executable implementation plan is finalized.
