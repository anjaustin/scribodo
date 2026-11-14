# Answer cycle 4: the default path (2026-11-14)

The author's direction: "Let's make sure all the best code and features are
on the default path." Before any default moved, every experiment flag's dated
record was read for what it measured and what the author decided; then the
one recorded blocker was measured; then two defaults moved and the rest were
confirmed where they are.

## The audit of the flags

Twelve fields in `src/core/experiments.h`, each read once from the
environment. A plain launch before this step took the original path: no
local memory, no verified pipeline, no reranker server, the observer's
step 4 code. What each flag's record says:

| flag | default before | measured verdict | record |
|---|---|---|---|
| `answer_parts` | off | worse: correct answers shown 3 to 1, wrong attributions 1 to 2; "Rejected" | answer-cycle3_relation.md |
| `answer_block_in_message` | off | the flag selects the worse shape (3a, 2 correct) over the default (3b, 3 correct) | answer-cycle2_pipeline_live.md |
| `memory_trace` | off | diagnostic; requests byte-identical with it on | answer-cycle4_flags.md |
| `memory_excerpts` | off | failed: 10 of 14 against a 14 of 14 target | MEMORY-022 |
| `memory_small_units` | off | failed: 11 of 14, one discovery lost, indexing twice as slow | MEMORY-022 |
| `semantic_debug` | off | diagnostic | TESTING.md |
| `memory_experiment` | off | retention done; selection 0.94 and 0.98 synthetic, 0.71, 1.00, 0.80 on three novels; sustained use steady over two hours; the retrieval answer path failed faithful use (MEMORY-009, MEMORY-011) | LANDSCAPE rows 11 and 12, MEMORY-019 |
| `memory_briefs` | off | failed: zero briefs published, 1 of 20 | MEMORY-009 |
| `answer_pipeline` (`=verified`) | off | better: 3 correct shown and 0 fabricated against production's 1 and 3 on the first novel; 0 fabrications shown in any recorded run on three novels; "Accepted as the candidate design"; held back for cost (10 to 15 s of reranking a question) and an unmeasured reranker memory figure | answer-cycle2_pipeline_live.md, MODEL-002, REDTEAM-021 R4 |
| `memory_presentation_compact` | off | 3 of 14 against legacy 1; all three failed the screen; archived | MEMORY-011 |
| `memory_presentation_episode` | off | 7 of 14, best of three, failed the screen; archived | MEMORY-011 |
| `observer_conflict_gate` | off | worse: off-fault conflicts 85 to 144, Mars recall 5 to 3 | observer-cycle2_quality.md step 5 |

So the flags were already at their measured best except two: local memory,
whose retention and selection are done and whose only failed part (the
retrieval answer path) is exactly what the verified pipeline replaces; and
the verified pipeline, measured better on every quality term it predeclared
and held off the default by cost and by one unmeasured figure.

## The blocker, measured

REDTEAM-021 R4 registered the reranker's memory figure as a 1 GiB guess
"required before the pipeline is anything but an experiment flag". Measured
today with the app's own arguments (`-c 2048 -b 2048 -ub 2048 -fa off -ctk
bf16 -ctv bf16 --cache-ram 0`, `build/llama/bin/llama-server`, the pinned
Qwen3-Reranker-0.6B-bf16 file of 1,142 MiB), from llama-server's own
breakdown: KV 224 MiB, Metal compute 360 MiB, CPU compute 64 MiB, 1,720 MiB
of device memory projected with the weights; the process's physical footprint
(`footprint`, `vmmap --summary`) 342 MB at ready and after one scored pair.
Beside the weights that is 578 MiB of device memory plus the process, under
the 1 GiB the fit subtracts; the figure stands, now as a measurement, and the
register entry closes.

The cost of the pipeline is recorded, not re-measured here: about 0.15 s a
pair, up to 64 pairs a question, so 10 to 15 s of reranking and 4 to 7 s of
extraction before the reply (MODEL-002, pass 6), on top of a first token that
with memory on is 3.7 to 20.5 s away at 3,138 to 13,179 prompt tokens
(MEMORY-019). The memory-on first-token budget row is still the author's to
set.

## The decision and what it changes

Both defaults move, with the author's per-conversation consent left as the
switch the author already has:

- `SCRIBODO_MEMORY_EXPERIMENT` is on unless set to `off` or `0`. A plain
  launch creates the document cache, the memory job and the generative gate
  and shows the right-click memory controls on stored chats; nothing is read
  into a request until the author turns memory on for a conversation
  (`memory_enabled` in chat.c, granted by the author's action, never by the
  variable).
- `SCRIBODO_ANSWER_PIPELINE` is on unless set to `off` or `0`. With a real
  model the launch starts the reranker server beside the chat and embedding
  servers, and the context fit subtracts its measured need. The pipeline
  runs only in conversations with memory on (`build_memory` is gated by
  `memory_enabled`), so a conversation without memory keeps today's request
  and today's speed byte for byte; one with memory on gets the verified path
  and pays its reranking time.
- A Mac without the reranker file does not fail to launch: the app says on
  stderr that answers are not verified and runs the plain composer
  (`main.c`, before the servers start). GAP-002's guided setup owes the
  install step.
- With the fake backend (UI scripts, `SCRIBODO_LLM=fake`) the pipeline runs only
  when asked for with `SCRIBODO_ANSWER_PIPELINE=verified`: its fixture scores
  but nothing extracts a span, so a memory-on turn would be rebuilt around "No
  verified passage"; the first gate run after the flip showed exactly that in
  seventeen store-UI assertions, and the scripts keep testing the recall path.
- `flag_on` in experiments.c is the default-on reading (only `off` and `0`
  turn a feature off); `test_core` pins the table: both on with nothing set,
  off under `off` and `0`, unchanged under other values.

What did not move: the eleven other fields, each at its measured default or
a diagnostic; briefs stay off; the archived presentations stay archived; the
conflict gate stays off.

## Gates

On the tree with this change and the REDTEAM-022 remediations
(`build/observer/cycle3d-gates/`): `make test` 95,766 C checks and 192 Python
tests, 0 failures; `make dbtest` 18,006, 0; `make crashtest` 133 rows, 129
deaths, 0 failures; `make uitest` 409 assertions, 0 failures, with memory on
and the pipeline's fixture held back under the fake backend; `make uitest-db`
281 assertions, 0 failures; `make analyze` 0 warnings across 87 files; `make sanitize`
95,766, 0; `make tsan` 48,339, 0; every exit status 0. The owed measurement is
a real-model session on this Mac with memory on in a conversation: the launch
footprint with the three servers (the fit's figure and the chosen context) and
the time to first token with the pipeline against `SCRIBODO_ANSWER_PIPELINE=off`,
to set the memory-on budget row.
