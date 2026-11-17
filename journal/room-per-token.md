# Room per token (2026-11-17)

**Question (the author):** can the chat model's memory per token of context come down, with the BF16 rule kept
(flash attention off)? Options measured: a smaller micro-batch, and checkpoints in place of `--swa-full`.

**Tool:** `tools/model_room_compare.py` (pinned llama-server `5851a8e5`, LFM3-1.7B BF16, this M4 with nothing else
running). Commands, output in `build/room/` (archived in `scribodo-evidence-2026-11-17-room.tar.gz`, `-room-b` and `-room-c`, docs/EVIDENCE.md):

```
T=tools/model_room_compare.py; O=build/room
python3 -I $T $O A0 40960 25 0 -- -b 128 -ub 128 --swa-full
python3 -I $T $O A5 40960 25 0 -- -b 128 -ub 64 -cms 0 -ctxcp 16
python3 -I $T $O D0 40960 8 1 -- -b 128 -ub 128 --swa-full
python3 -I $T $O D5 40960 8 1 -- -b 128 -ub 64 -cms 0 -ctxcp 16
python3 -I $T $O M0 139264 1 0 -- -b 128 -ub 128 --swa-full
python3 -I $T $O M1 139264 1 0 -- -b 128 -ub 64 -cms 0 -ctxcp 16
python3 -I $T $O M2 16384 1 0 -- -b 128 -ub 64 -cms 0 -ctxcp 16
python3 -I $T $O --precision P0 -- -b 128 -ub 128 --swa-full
python3 -I $T $O --precision P0b -- -b 128 -ub 128 --swa-full
python3 -I $T $O --precision P5 -- -b 128 -ub 64 -cms 0 -ctxcp 16
python3 -I $T $O --compare P0 P0b P5
# red-team follow-up: fixed cost as macOS sees it, long prompts
python3 -I $T $O F5s 16384 30 0 -- -b 128 -ub 64 -cms 0 -ctxcp 16     # also F5sb
python3 -I $T $O F5 40960 30 0 -- -b 128 -ub 64 -cms 0 -ctxcp 16      # also F5b, F5c
python3 -I $T $O F0 40960 30 0 -- -b 128 -ub 128 --swa-full
python3 -I $T $O --long L0 5 -- -b 128 -ub 128 --swa-full
python3 -I $T $O --long L5 5 -- -b 128 -ub 64 -cms 0 -ctxcp 16
python3 -I $T $O --compare L0 L5
python3 -I $T $O G5 40960 4 2 -- -b 128 -ub 64 -cms 0 -ctxcp 16 --load-mode mmap+mlock   # also G5b
```

## Memory (KV + recurrent state + Metal and CPU compute, as allocated)

| Run | Cells | MiB | KiB a token |
|---|---|---|---|
| M0, before (`-ub 128 --swa-full`) | 139,264 | 2,993.2 + 1,371.5 = 4,364.7 | 32.1 |
| A0, before | 40,960 | 881.2 + 411.5 = 1,292.7 | 32.3 |
| M1, now (`-ub 64 -cms 0 -ctxcp 16`) | 139,264 | 1,640.7 + 618.1 = 2,258.9 | 16.6 |
| A5, now | 40,960 | 488.7 + 186.1 = 674.9 | 16.9 |
| M2, now | 16,384 | 200.7 + 78.1 = 278.9 | 17.4 (a 15 MiB intercept) |

Slope between M2 and M1: 16.5 KiB a token; 16.75 counting the CPU buffer twice as llama.cpp's projection does. The app
uses 17 (`llm_server_mem_need`).

## Fixed cost, as macOS sees it

What can be handed out (the app's own measure) before the server starts, once it is ready, and after a 30-turn chat
with a slot-1 request each turn (runs F, the tool's TURNS mode; model file 3,306 MiB):

| Run | Cells | Taken, ready | Taken, after the chat | Estimate at 384 fixed |
|---|---|---|---|---|
| F5s, F5sb (now) | 16,384 | 3,689, 3,695 | 3,629, 3,360 | 3,962 |
| F5, F5b, F5c (now) | 40,960 | 4,072, 4,074, 4,074 | 4,333, 4,165, 4,332 | 4,370 |
| F0 (before, at 34 KiB) | 40,960 | 4,689 | 4,908 | 5,050 |

Ready, the server takes the file plus its buffers plus 92 to 93 MiB (three runs at 40,960 cells), 104 to 110 at 16,384. A chat adds up to 260 MiB more: its checkpoints
(up to 16 a slot, 3.1 MiB each) and buffers llama.cpp grows as it works. At 384 MiB fixed, the estimate held by 37 MiB
at 40,960 cells, against 142 for the old flags. So the fixed cost is now 512 MiB.

The F runs filled only about 8.4K of the pool and ran without the app's `--load-mode mmap+mlock`, and their "after"
readings spread over 168 MiB at 40,960 cells. So two more runs (G5, G5b) used DOC=2: about 25K tokens of the novel in
the newest message, four turns, the prompt reaching 26,257 of 40,960 cells, with `mlock`.
- Taken ready: 4,145 and 4,074 MiB.
- Taken after: 4,155 and 4,296 MiB.
- Estimate at 512 fixed: 4,498, a margin of 202 to 343 MiB before the app's own headroom.
- Each turn read the whole prompt again: the document travels with the newest message.

Taken one at a time earlier the same day (scratch runs with the harness the tool came from, kept under
`build/room/scratch`): `-ub 64` alone took the compute buffers from
412 to 206 MiB at 40,960 cells (27.2 KiB a token); dropping `--swa-full` alone took the KV from 880 to 488 MiB (21.5).

## Cache reuse

A 25-turn chat (a paragraph and a question a turn, 160-token greedy replies, a bounded slot-1 request between turns):
- A0 read about 150 tokens a turn (last eight: 170, 147, 246, 211, 97, 92, 84, 77), 5.75 s of prompt time in all.
- A5 read about 550 (444, 473, 549, 613, 464, 715, 588, 568), 17.0 s; 21.0 in the scratch run. The checkpoint at the
  previous user message must lie at least the window (256 tokens) before the point where the prompts part, so the
  turn reads the last exchange or two again.
- Without `-cms 0` (scratch D2), a chat with a chapter attached read its whole prompt every turn: llama.cpp's default
  spacing of 8,192 tokens left no checkpoint early enough.

Eight turns with an 8K-token chapter in the newest message (as the app attaches documents):
- D0 read 6,877–7,143 a turn, 81.9 s.
- D5 read 6,877–7,623 a turn, 85.8 s.
- The chapter is read again every turn either way, since it moves to the newest message.

In the app (`SCRIBODO_LLM=real`, the UI script `build/room/app/turns.scribo` driving a seven-turn chat with no
document, replies of about 150 words), the server read 114, 319, 436, 422, 421, 434, 296 tokens on slot 0 (0.3–0.9 s),
as the conversation grew (`build/room/app/turns-server.txt`; an earlier run, its lines not kept, printed the same counts).
- A scratch run with a 30,000-byte chapter open and attached read 6,828–7,126 tokens in each of six turns (9.3–11.1 s),
  the chapter again each time. Its log was not kept; recorded here only.

## Long prompts

Five prompts of about 25,000 tokens (`--long L0 5`, `--long L5 5`, no cache):
- Reading took 49.5–50.4 s with the old flags and 43.9–44.2 s with the new.
- Over 120 identical-context steps, the largest top-10 difference was 0.0209.
- One top token changed (' Long' to ' Nether'), where the base's top two were 0.446 and 0.444.

## Output

Greedy, 24 tokens on each of A0's 25 turn prompts, top 10 log-probabilities, compared up to each prompt's first
differing token:
- P0b against P0: 600 steps, largest top-10 difference 0.0000, no change of top token. The old flags are
  deterministic.
- P5 against P0: 600 steps, largest difference 0.0240, no change of top token.

Every kernel stays BF16 or F32 (flash attention off, BF16 K and V); only the summation order changes, as with the
earlier micro-batch and `--swa-full`.

## Launch

With the author's 128K setting and the same three servers (chat, embedding, reranker):
- Before the fixed cost was raised, the app started at 56K (`build/room/app/launch-384.log`: "a 128K context needs
  about 6.3 GB and 5.1 GB is free for it").
- With 512 fixed, it started at 40K, the pool at 49,152 (`build/room/app/launch.log`, `launch-flags.txt`: "needs about
  6.4 GB and 4.9 GB is free").
- On those 4.9 GB, the old flags' estimate (34 KiB a token, 384 fixed) would have allowed 16K.
- That morning the app had started at 8K (`build/room/app/launch-morning.log`).
