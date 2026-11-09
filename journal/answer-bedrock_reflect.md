# Working model and falsification plan

Core testable distinction: evidence present in JSON is not necessarily evidence
the model can use when it generates the next token.

Two competing explanations remain open: request/context organization overwhelms
otherwise working inference; or serving/template/cache behavior corrupts inference
even on appropriately bounded context. The minimal grounded control tests the
second explanation first. Identical full requests with and without cache reuse
test serving state independently of the source-selection policy.

Removing history and document heads independently tests their contributions.
Moving the unchanged source packet after the heads tests evidence position
without adding stronger instructions or changing the answer key. Deterministic
decoding isolates these effects from sampling; an otherwise identical sampled
arm checks whether the original symptom depends on decoding.

Important constraint: original wire requests were not retained. Diagnostic
reconstruction therefore uses production preparation and the frozen prior
visible replies, with generation replaced only in a generated test build.
Retrieval ties may differ in the fresh database. Conclusions must refer to these
captured reconstructed payloads, not claim exact reproduction of unknown bytes.

The runtime's documented 256-token sliding attention makes evidence position a
specific hypothesis worth testing, not a proven explanation. Prior numerical
correctness out to 4K tokens does not establish useful factual recall at the
application's actual request lengths. No production change follows from this
observation alone.

Config inspection narrows that statement: the pinned model has **six full
attention layers as well as sliding attention and convolution layers**. Thus
256 tokens is not its total attention horizon. A strict-window explanation is
rejected by the architecture itself; the proposed ordering ablation remains
valid but cannot be explained as all earlier evidence being inaccessible.
