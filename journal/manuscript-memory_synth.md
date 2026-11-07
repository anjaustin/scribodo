# Manuscript memory — recommendation

See [`../docs/MEMORY-001-long-lived-context.md`](../docs/MEMORY-001-long-lived-context.md)
for the source-pinned feasibility assessment and staged P0 plan. The five product
gaps are retained in [`../docs/PRODUCT-GAPS.md`](../docs/PRODUCT-GAPS.md).

- Build/evaluate a bounded native context planner before adopting the full proxy.
- Keep original messages/revisions, hierarchical briefs and exact retrieval as
  separate layers; summaries never become canon or replace original text.
- Address host paging/caches/incremental work as well as model token limits.
- Start with representative scale measurements and golden recall/privacy cases;
  compare current behavior, retrieval-only and summary-plus-retrieval variants.
- Treat the upstream benchmarks as useful external evidence, not a guarantee for
  LFM3-1.7B BF16 on an M4/16 GB Mac.
- No upstream package was installed or executed, and no inference behavior was
  changed. Implementation and final numeric budgets remain a next-step decision.
