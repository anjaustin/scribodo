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

## Measured on this Mac, the default path with the real model (2026-11-14, evening)

**Launch footprint, three servers** (`build/ui/fit-on`, the fixture project, idle
machine, 6.9 GB free-ish before launch by `vm_stat`): LFM3 chat server RSS
3,598 MiB, embedding server 1,481 MiB, reranker 1,440 MiB (physical footprints
305, 336 and 307 MB; the rest is the mapped weights and Metal buffers in unified
memory); the app 74 MiB. The fit's line: "the model needs about 4.2 GB and 4.0
GB is free for it. It's starting with a 8K context anyway". **The chat server
started with an 8K slot.** The same launch with `SCRIBODO_ANSWER_PIPELINE=off`
(two servers) printed no fit warning and started with a 32K slot
(`~/Library/Logs/Scribodo/llama-server.log`, `n_ctx_slot = 8192` and `32768`
alternating with the two arms). That is the cost of the reranker being resident
at launch on a 16 GB machine beside the 3 GB Docker VM: the plain composer's
context is a quarter of what it was. The author decides whether that trade
holds; the alternative is a reranker started on demand, which moves the memory
pressure to the first verified question instead of removing it.

**The 40-minute soak with the observer on, pipeline on** (`build/observer/
default-path/soak-pipeline-observer-40.log`, `tools/history_frame_soak.sh
--manuscript --memory --real --observer --minutes 40`): 23 cycles, 124 of 124
checks, but the run took 581 s because every send was short: the chat server
saw 23 prompt evaluations of 321 to 515 tokens (488 to 871 ms) and generated 42
to 192 tokens a reply; the reranker scored nothing in the whole run. The
soak's conversation has memory on and no documents attached, so the pipeline
found no document unit, wrote "No verified passage was found" into the
request, armed the gate, and the composer abstained on every question; the
recall plan, which would have answered from the chat's own history, was not
taken. **Fixed the same evening in software:** `memory_pipeline` returns to the
recall path when the pipeline's status is `no_candidates` (nothing to verify is
not a search result), with the notice "no document passages to score; recalled
from the chat instead". The observer under wait mode: 7 cancelled calls in the
server log against one per send before (35 in 35), and `observer_lag` equal to
the message count at every sample as before: the soak's 14-second pauses never
reach the 24 s the backoff asks for after two cancellations, so the observer
spends no model time on work the next send discards and makes no progress until
the author pauses longer. Frames: 25,922 at p50 17.75, p99 23.0, worst 65.1 ms,
6 above 50 ms (0.02 percent).

**The 15-minute soak with the pipeline off** (`soak-plain-15.log`): 9 cycles in
835 s, 713 of 713 RSS samples; the chat server saw 9 prompt evaluations of 3,104
to 13,299 tokens (3.7 to 18.3 s, median 6.5 s at 5,267 tokens) and generated up
to 2,048 tokens a reply; frames 38,378 at p50 18.25, p99 35.0, worst 51.0 ms, 2
above 50 ms. This is the plan path's known shape (MEMORY-019: 3.7 to 20.5 s to
first token).

**The boundary questions over *Pride and Prejudice* with documents attached**
(`tools/answer_product_run.py defaultpath-pp`, the pipeline on,
`build/product-memory-use/defaultpath-pp/`): 21 messages, every assistant turn
with a receipt and a verified source (scores 0.36 to 0.9992); the gate passed 3
(the walk's "only three miles", the sisters' fortune, the two drafts' "three
miles; five miles"), withheld 2 as echoing the question, abstained on 2, and
did not arm on the two notes and the creative request. The reranker scored 450
pairs in 103.9 s (231 ms a pair), about 10 s a question; the chat server saw 38
prompt evaluations of 168 to 576 tokens (0.26 to 0.79 s; one of 2,185 tokens at
2.6 s) for the ten sends and their extraction and claim calls, against the plan
path's 3,104 to 13,299 tokens: the verified request is a tenth the size and
reaches its first token later, about 12 to 15 s after the send instead of 3.7 to
18 s (median 6.5). The script's four `expect memory_sources` failures are the
plan path's source-delivery checks (the designated passage text); under the
pipeline the receipt carries the verified span instead, as in every recorded
pipeline run. The author's memory-on budget row has its numbers: reranking
about 10 s a question plus one extraction call, then a sub-second prompt.

**The fallback, verified** (`soak-fallback-5.log`, `server-soakC.log`: the same
memory-on conversation with no documents, five minutes, the fix built): 3
cycles, 24 of 24 checks, and the chat server saw three prompt evaluations of
2,997 to 3,014 tokens (3.5 to 3.7 s) with replies of 273 to 2,048 tokens: the
recall plan answered from the chat's history again, as before the flip.

Gates on the tree with the fallback (`build/observer/cycle3d-gates/`, third
run): `make test` 95,766 C checks and 193 Python tests, 0 failures; `make
dbtest` 18,006, 0; `make crashtest` 133 rows, 129 deaths, 0 failures; `make
uitest` 409, 0; `make uitest-db` 281, 0; `make analyze` 0 warnings across 87
files; `make sanitize` 95,766, 0; `make tsan` 48,339, 0; every exit status 0;
the fake-path yardstick byte-identical with `realflag` on three novels. Hosted: 43630415936 CI success; 43630415947 Trusted acceptance success on `e1482c5`.

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
to set the memory-on budget row. Hosted: CI and Trusted acceptance passed on
`52d44fe` with `8c6efc9` (runs 43621647190 and 43621647088).
