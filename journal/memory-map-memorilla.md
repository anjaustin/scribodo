# Memorilla as a map to the manuscript: measured, rejected (2026-11-15)

**Question.** Memorilla (Stanford SNAP et al., arXiv 2610.21375, code `snap-stanford/memorilla` at `d3e3f8f`)
compresses a document collection into 16 query-conditioned soft tokens for a frozen Qwen3-8B. Its 16 slots attend over
every document embedding. Could that attention serve as a *map*: rank the passages a question needs, so the verbatim
text is fetched from the store and the model answers from exact, citable words (MEMORY-015), rather than from
Memorilla's soft tokens (which the paper says "can lose fine-grained factual details")?

**Method** (`tools/memorilla_map_probe.py`; setup in its header; nothing installed into the product). The 44
hand-written reach questions over the three novels (`tests/fixtures/memory_reach/*-tasks.json`), each with the one
sentence that answers it. Each novel's 8 part files cut into units the product's way in size (paragraphs packed to
2,000 bytes, an oversize paragraph at sentence ends): 212, 415 and 396 units, every question with exactly one
answering unit. Every arm ranks every unit for every question. The released `stage3` module (2 cross-attention layers,
8 heads, 16 slots, width 2,560) over Qwen3-Embedding-4B vectors computed the way the paper's vLLM path does (last-token
pooling, L2-normalised, no instruction), on the Mac's GPU through `transformers`. The primary score, fixed before the
run: each unit's attention summed over both layers, all heads and all slots.

| arm (44 questions) | rank 1 | top 8 | top 32 | MRR |
|---|---|---|---|---|
| Qwen3-Embedding-0.6B, the product's query instruction (the product's semantic channel alone) | 14 | 30 | 40 | 0.44 |
| Qwen3-Embedding-0.6B, no instruction | 14 | 28 | 37 | 0.44 |
| Qwen3-Embedding-4B, no instruction (Memorilla's own vectors) | 19 | 33 | 38 | 0.53 |
| Qwen3-Embedding-4B, the product's instruction | 18 | 34 | 39 | 0.54 |
| **Memorilla attention, both layers (primary)** | **0** | **1** | **3** | **0.02** |
| Memorilla attention, layer 1 / layer 2 | 0 / 0 | 1 / 2 | 5 / 7 | 0.01 / 0.02 |

Per novel and per question in the tool's output (`build/memorilla/run1.txt`). The answering unit's median rank under
the module's attention is about 180 of about 340: chance.

**The readout is the module's own computation.** `read_cache` and `forward` agree to 6.5e-5, and the attention path
the probe reads, carried through to the memory tokens, reproduces `read_cache`'s tokens exactly (difference 0.0). The
inputs are unit-normalised as vLLM's are.

**Why.** The attention is sharp (mean entropy 0.2 nats in layer 1 and 0.5 in layer 2 against 5.4 to 6.0 for uniform:
each slot settles on about one unit) but not on the question's evidence. In layer 2 two questions share 6.4 of their 8
most-attended units; the slots gather a broad, mostly question-independent sample of the book, and the decoder reads
what it needs out of the mixture. The module was trained only through the decoder's answer loss, never to point, so
nothing made its attention a relevance ranking. That is consistent with the paper's own limitation and with its loss
to MemoRAG on NarrativeQA.

**Decision.** Memorilla is not a map, and its soft tokens cannot carry the author's exact words, so neither use fits
the contract. Not adopted; the Observer work resumes. Row 12 of the landscape records it.

**One side result, not acted on.** The 4B embedder ranks the answering unit first for 19 of 44 questions against 14
for the product's 0.6B, though top-32 reach is the same (38 against 40) and the product adds a lexical channel and a
reranker on top (reach@candidates 1.00 on two novels, journal/answer-cycle4_mars.md). It costs 7.5 GB of weights (it
would not fit beside the three model servers on this 16 GB Mac) and embedded 6 times slower (542 s against 87 s for
*Pride and Prejudice*). A question for a larger machine, not for now.
