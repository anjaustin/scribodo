# MODEL-002: Qwen3-Reranker-0.6B as the verified-answer pipeline's selector

**Date:** 2026-11-10. **Status:** pinned runtime; on the default path since 2026-11-14 for conversations with memory on (journal/answer-cycle4_default_path.md; its memory figure measured there, 1 GiB beside the weights confirmed).

## Why a second specialist

Four measured cycles on the eight boundary questions (journals
`answer-cycle2_clean_review`, `_unit_extraction`, `_ranked_span`, `_rerank`)
showed the local LFM3-1.7B cannot select which unit answers a question at any
granularity, that the production embedder ranks short stating units and spans
last under any query instruction, and that a cross-encoder reading question
and unit together places every designated unit in delivery range (documents
only: the three retrieval misses at ranks 1, 3 and 4 against the embedder's
25, 18 and 70). The pipeline therefore ranks units with a cross-encoder,
extracts one span from the top unit, byte-verifies it, and composes from that
span alone.

## The model

| | |
|---|---|
| Source | `Qwen/Qwen3-Reranker-0.6B`, Hugging Face revision `e61197ed45024b0ed8a2d74b80b4d909f1255473` |
| License | Apache-2.0 (model card) |
| Weights hash (safetensors) | `27cd75a405b9c1b46b59abfd88aaa209e6fed2a1972cde9b70e7659537c5e65b` |
| Conversion | `third_party/llama.cpp/convert_hf_to_gguf.py` at the pinned llama.cpp commit, `--outtype bf16`; the model is a plain Qwen3 causal LM, so it converts without a reranker-specific path |
| GGUF hash (pinned in `src/llm/server.c`) | `5a6bf98df1ba7b7a957bbcfc14e74a73b4c314812c2224fe048ea942db4e53f1` |
| Install path | `~/Library/Application Support/Scribodo/models/Qwen3-Reranker-0.6B-bf16.gguf` (`SCRIBODO_RERANK_MODEL` overrides) |
| Server | `llama-server -ngl 99 -fa off -ctk bf16 -ctv bf16 -np 1 -c 2048 -b 2048 -ub 2048 --cache-ram 0`, own run directory, key and log |
| Scoring | the model card's prompt and default instruction; `/completion` with one predicted token and the top 20 probabilities; P(yes) renormalized against P(no) |

Precision: BF16 weights and BF16 KV through the same validated kernel set
as MODEL-001 (`-fa off`). The precision gate verifies the GGUF hash once and
caches it like the other two models.

## Parity with the reference (build/rr1)

89 fixed pairs (29 question-span, 9 designated units, 40 sampled units) scored
by llama-server (BF16) and by the PyTorch float32 reference used in the
ranking measurements:

| | |
|---|---|
| Maximum absolute score difference | 0.026 |
| Mean absolute difference | 0.0034 |
| Pairs within 0.01 | 82 of 89 |
| Threshold-0.5 decision agreement | 89 of 89 |
| First place per question | 8 of 8 agree |
| Top-10 overlap over all pairs | 10 of 10 |
| Seconds per pair | 0.154 llama-server, 0.223 PyTorch |

Limit: when one of "yes"/"no" is outside the top 20 probabilities its share is
taken as zero; this happened on 53 pairs and the resulting differences are
inside the figures above.

## Cost

About 0.15 s per pair on this Mac. The pipeline scores up to 64 candidates
per question, so selection alone is up to 10 s before extraction and the
reply. That is why the pipeline was an experiment behind
`SCRIBODO_ANSWER_PIPELINE=verified` with its own 90 s deadline, not a
foreground default: the live measurement records the wall time with the
answers. On 2026-11-14 the author put the measured-best paths on the default
(journal/answer-cycle4_default_path.md): the pipeline answers whenever a
conversation has memory on, which the author grants per conversation, so the
cost is paid only where the author asked for memory; `=off` selects the plain
composer.

## Saturation

The reranker's scores saturate near 1.0 when several passages carry the
question's names and content words, and the one that states the asked
attribute can rank under them by a few thousandths. Two instances are
measured, one per novel: the sisters' fortune (first novel; the stating
clause has a pronoun subject, the unit ranks 5 to 7) and Kantos Kan's rank
(second novel; the stating sentence names him in an appositive, the unit
ranks 7th at 0.9913 under six units within 0.006 of the top). Every local
route is measured and closed: per-part reranking, a named-entity admission
filter, sentence windows, embedder order within the reranker's tie, rank
fusion, and a larger reranker (not possible beside the chat model on the
MVP machine). The embedder is right where the reranker is wrong on the first
instance and wrong on the second (32nd of 43); windows reach neither.
`journal/answer-cycle3_relation.md` and `journal/answer-cycle4_margin.md`
hold the tables; `tools/answer_margin_arm.py` reruns the second on any
recorded run. The fixture cases are permanent regression probes.

## Not claimed

Reranker quality beyond the measured eight questions; two-value questions
(the second unit ranks 5 to 7); chat-unit hygiene (the session's own turns
outrank manuscript units under every ranker measured, including this one).
