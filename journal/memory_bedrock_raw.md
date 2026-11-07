# Memory-quality investigation: evidence and open questions

This is an investigation log, not a claim that retrieval or the model is fixed.
The frozen V2 evaluation and its negative results must remain unchanged. The
new probes are diagnostic experiments, not replacement held-out quality scores.

Observed: the 115,001-word run selected expected original sources but answered
only one of twenty questions correctly in each retrieval arm. Oracle-original
selection also failed. T1 generated no accepted brief; its first attempt consumed
the entire 256-token output allowance. Some answer responses emitted malformed
JSON, irrelevant recent-history content, or abstention despite supporting text.
No emitted private canary was observed in that bounded experiment.

Unknown: whether the actual rendered prompt contains every requested message;
whether consecutive user messages are rendered differently from ordinary
alternating history; whether the pinned model can perform a single-fact lookup
without history; whether special-token/reasoning configuration affects content;
whether output budgets truncate necessary structured output; whether previously
used server slots affect independent requests; whether strict generated schemas
are feasible on this checkpoint; and whether summary generation fails because
of content, schema, runtime compatibility, or truncation.

Constraints: keep model identity and production launch flags explicit; use a
private local Unix socket and owned subprocess; do not use the author's running
model or any remote provider; do not alter frozen fixtures or scoring. Capture
raw requests, rendered prompts, responses, finish reasons, usage and versions.
Use fresh diagnostic names/facts, rather than learning answers from held-out
cases. Compare one change at a time and preserve negative results.

First tests should establish checkpoint/template integrity and a minimal fact
baseline, then introduce source framing, repeated history, role layout and JSON
requirements. A failed simple baseline must be investigated below retrieval;
a successful simple baseline followed by history failure localizes the issue
to composition or runtime state rather than proving a model-capability limit.
