# Manuscript memory — findings and decision points

## Observations

1. Primary upstream: `ranxianglei/billion-context`, inspected at
   `c3dfbe6b21a1b2fc3642594043bc81fd2ae91247`; package declares 0.1.212.
2. The design is model-authored hierarchical summaries with retained originals,
   range identities, search and restoration. It does not expand model attention.
3. The TypeScript kernel is host-independent and performs no inference or I/O;
   the host must supply summaries, persistence, token counts and integration.
4. Both inspected licenses contain MIT text plus an additional visible
   attribution term. Package metadata saying MIT is not the whole license.
5. Published benchmark results use different, much larger/different models than
   Scribodo and an older package version. They are not local-model validation.
6. Upstream explicitly reports host-history slowdown despite compressed model
   requests. Scribodo also has all-history copies and per-message UI caches.
7. Scribodo already owns authoritative text and derived indexing; creative chat
   currently retains a recent window and cuts attached documents to fit.

## Design dependencies and tensions

8. Compression reduces prompt cost but can lose exact details. Retained raw
   sources and exact retrieval are necessary; re-summarization is not recovery.
9. A bounded prompt does not imply bounded application RAM or per-turn work.
   Paging, capped caches and incremental jobs are separate requirements.
10. Native reuse avoids a TypeScript sidecar but adds implementation work. Direct
    reuse adds runtime, transport and state-integration obligations.
11. Existing observer separation and local/remote consent must survive memory
    construction. Summaries inherit source eligibility, not broader access.
12. Small-model compression quality and tool behavior are unknown until measured
    with the pinned local BF16 model. A host-scheduled summarizer avoids depending
    on autonomous tool use for the first experiment.

The design has real trade-offs, so the investigation continues beyond a simple
reuse/no-reuse answer. Source links and the actionable recommendation live in
`docs/MEMORY-001-long-lived-context.md`.
