# Manuscript memory — decision summary

**Core conclusion:** Keep a potentially large addressable source archive, while
bounding both the model's working set and the application's per-turn work.

Hierarchical summaries and retrieval serve different tasks. Summaries retain
continuity and intent; exact source lookup retains quotations, names, provenance
and corrections. The upstream benchmark's exact-retrieval trade-off rules out
summary-only memory as the recommendation for this writing application.

The upstream FAQ also rules out a second assumption: adding compression does not
by itself resolve GAP-003. Scribodo's request worker copies all conversation
content before trimming, and the chat UI maintains per-message text/layout caches.
Those are scaling candidates even when inference input is already bounded.

The preferred evaluation path is a small native context planner using existing
MariaDB/Qdrant ownership, incremental summaries and bounded exact-source reads.
Compare this against existing behavior and a summary-free retrieval baseline.
Keep kernel/sidecar reuse as an alternative if measured maintenance benefits
outweigh the added runtime and adapter complexity. No implementation is selected
solely from upstream benchmark claims.
