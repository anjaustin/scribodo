# Memory MVP — scope, alternatives and open questions

This pass applies the four phases in `anjaustin/lmm` at
`55719c0a2308d1a3f29ca656ed7b6287be33bca2`, with an additional adversarial
verification loop before synthesis. It is a design record, not a transcript of
private deliberation. GAP-003 remains P0; unrelated product gaps remain deferred.

The product goal is a writing session that remains useful after the transcript
outgrows the model window, without manual resets or repeatedly explaining the
book. The MVP must preserve the actual conversation, offer useful continuity,
recover precise supporting text, and remain responsive on the supported M4/16 GB
machine. Preserving every source and answering every conceivable historical
question are different capabilities; both continuity and exact-detail behavior
need measured acceptance tasks.

Several implementation paths remain plausible: reuse the full proxy, wrap the
TypeScript kernel, or build a native bounded planner. The existing C application
already owns MariaDB authority, Qdrant-derived retrieval and a supervised local
BF16 model. Reusing these services avoids a second authoritative transcript, but
requires explicit orchestration rather than assuming vector search is memory.

A naive proposal would store summaries in MariaDB, embed them in Qdrant, and send
the nearest results. Its risks are stale vectors being treated as current facts,
forgotten exact details, SQL/vector dual-write gaps, background jobs publishing
against changed sources, and all-history copies remaining in the UI/request path.
Another risky shortcut is recursively compressing everything into one mutable
recap; that loses independent provenance and makes corrections hard to isolate.

Open questions: What is the smallest hierarchy that actually adds value beyond
retrieval alone? Which sources are eligible without expanding author consent?
How does a partially processed transcript avoid a falsely advanced coverage
cursor? What happens when Qdrant is unavailable but MariaDB is healthy? What
distinguishes a canceled/retried job from an authorized publication? How are token,
byte, candidate and cache limits enforced when documents or replies are large?
Can the pinned 1.7B model write useful summaries without expensive foreground
compaction or a hidden larger model? These questions must become explicit gates,
not assumptions concealed by a successful small demonstration.
