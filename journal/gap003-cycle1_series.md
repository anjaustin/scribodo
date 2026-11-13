# GAP-003 cycle 1: the baseline as a series, budgets proposed, the soak instrumented

**Date:** 2026-11-13. **Tree:** `0d95ad2` plus this cycle's tooling.
**Direction:** "Let's make sure we've remediated thoroughly, the proceed with the
next P0." P0 working item 5 (docs/PRODUCT-GAPS.md): "Workload sizes and
budgets agreed, then the soak, restart and repeatability runs on the M4; the
single MEMORY-019 baseline becomes a series." Measure, always; guess last.

## Method

Nothing in the product changed. Three tools and one script:

- `tools/manuscript_series.sh [--runs N] [--out DIR] [-- baseline args]` runs
  `tools/manuscript_baseline.sh` N times, each on a fresh disposable database
  and project, keeps every `run-N.log`, and tabulates them with
  `tools/manuscript_series.py` (per phase: minimum / median / maximum of wall,
  p99 and worst frame; app RSS; service peaks; process wall). A run missing a
  phase or failing a check is named and left out of the spread; pinned by
  `tests/test_manuscript_series.py`.
- `tools/store_growth_sample.sh scribodo_test_NAME [LABEL]` prints one
  `STORE_GROWTH` line, read-only, through `tools/services.sh testdb sql`: the
  database's data and index bytes; message, unit, record and revision counts;
  the memory outbox by state (pending, leased, failed); the observer's lag
  (messages past each conversation's reviewed point); the enabled memory
  scopes' ingest cursor and pending sequence against the highest message
  sequence; the semantic collections' points.
- `tools/history_frame_soak.sh` gained `--memory` (the manuscript cycle with
  memory enabled over the whole chat first, `tests/ui/store/24_manuscript_soak_memory.scribo`),
  `--real` (the pinned model for the sends), `--cycle-seconds` (the measured
  cycle work the run is sized with: 2 s fake, 90 s real) and
  `--sample-seconds` (a `STORE_GROWTH` line every N seconds while the app
  runs, default 60). The watchdog budget includes the memory enable.

- `tools/manuscript_restart.sh [--cycles N] [--real]` runs the memory soak
  cycle until N cycles have landed, kills the app with SIGKILL mid-cycle,
  samples the store, and starts the app again on the same database and folder
  under `tests/ui/store/25_manuscript_resume.scribo` (open from the store,
  recall, a send, each measured); then waits for memory to settle on its own
  and checks the message count is the kill's plus the resume's two.

Every run on an idle machine (the hosted runner quiet, nothing else built),
the series under `build/gap003/series-fake-5` and `series-real-3`, the soak
smoke logs under `build/gap003/soak-smoke*.log`.

## The series

Fake model, five runs, 28 of 28 checks each; real model, three runs, 28 of 28.
The tables and their reading are in MEMORY-019 ("The baseline as a series");
the facts that matter here:

1. Every phase but the send is repeatable to a few percent of its median
   across eight runs (import 0.92 to 1.07 s, open chat 0.10 to 0.11 s, paging
   0.62 to 0.64 s, save 0.20 to 0.23 s, recall 0.28 to 0.30 s, memory enable
   53.97 to 54.26 s fake and 118.3 to 119.1 s real). The frame figures move
   within a frame interval except the save's and the fake send's worst frame
   (39 to 48 ms over ten and fifty-five frames).
2. The real send's wall was 5.67, 30.48 and 87.50 s. The model server's log
   (`~/Library/Logs/Scribodo/llama-server.log`): prompt evaluation 2.29, 2.45
   and 2.48 s for 1,854, 1,975 and 1,975 tokens; generation 41.5, 41.1 and
   41.1 ms per token; 2,048, 676 and 71 tokens generated. The 2,048 is the
   request's cap (`max_tokens` 2048 in the recorded requests; the chat's
   sampling profile `max_reply`, `chat_apply_sampling` in chat.c), so 87.5 s is
   the longest a send can take at this prompt size. The send budget is on time
   to first token and tokens per second; the wall follows from the cap.
3. Service memory as `docker stats` reports it is cgroup usage with the page
   cache: Qdrant 890 MB = 379 MB anonymous + 469 MB file cache (process RSS
   729 MB: 362 anonymous, 368 file-mapped); MariaDB 491 MB = 408 + 64. Limits
   1 GiB and 768 MiB. The soak samples the store itself rather than inferring
   growth from RSS.

## Budgets

Proposed in MEMORY-019 from the series (about twice the median where the
series is tight; the measured unit where the wall is not the right one):
worst frame 50 ms in any phase; p99 25 ms outside a streaming send and the
save; open chat 0.25 s; paging 1.0 s; save 0.5 s; recall 1.0 s; import 2 s;
memory enable 0.2 s per turn; time to first token 4 s at about 2,000 prompt
tokens; at least 20 tokens per second; app RSS 256 MB; service anonymous
memory 512 MB each. They await the author's agreement; the item says
"agreed", and that is not something a measurement can do.

## What the soak smoke found before the soak

The first smoke of `--memory` (one minute requested) failed its
`wait_memory 1116` after ten minutes and then ran its cycles clean. The
sampler showed the outbox drained (0 pending, 0 leased, 0 failed), 1,906 units
indexed, and, once it reported the scope cursor, `scope_cursor=1115
scope_pending=1115 max_seq=1115`: fully ingested and settled. The fixture's
1,116 turns are sequences 0 to 1115; the baseline's `wait_memory 1116` passes
only because its send appends sequences 1116 and 1117 before memory is
enabled. The soak script now waits for 1115, with the reason beside it. Not a
product defect: the settle predicate (`host_memory_settled`: cursor at or past
the sequence, nothing pending, no index work outstanding) was right, the
script's number was not. The baseline's own wait keeps working for the same
reason it always did. The corrected smoke (`soak-smoke4.log`, one minute
requested): 29 of 29 checks, four cycles, 3,443 frames at p99 19.3 ms and
worst 46 ms; the store grew from 1,116 messages and 1,906 units to 1,124 and
1,914 with the cursor at the last sequence after every cycle and the outbox
empty at every sample, 11.9 MB of data and index for the project.

The restart runner's smoke (`build/gap003/restart-smoke`, one cycle, fake
model): the app killed with SIGKILL at 1,118 messages while ingesting (the
sample before the kill shows the outbox mid-flight), the same database and
folder reopened from the store in 0.18 s (ten frames, worst 19.7 ms), recall
in 0.27 s, a send in 0.87 s, 11 of 11 checks; memory settled on its own
(cursor 1119 at max sequence 1119, outbox empty); messages 1,118 before and
after the kill, 1,120 after the resume's question and reply. One run, a
smoke; the measured restart runs follow in this cycle.

Two observations recorded, not acted on:

- With the fake model the observer reviews nothing (`observer_lag` stays at
  the message count throughout), so the fake soak measures ingestion and
  indexing backlog, not observer backlog; the real soak measures both.
- MariaDB holds nine `scribodo_test_*` databases from earlier runs that were
  killed before their cleanup (about 25 MB in all) and Qdrant one leftover
  test collection; disposable state on the development Mac, listed here so
  the service memory figures are read with it in mind. Dropping them is a
  housekeeping decision, not taken here.

## Gates and verdicts

No C source changed in this cycle (the tree's C is `0d95ad2`'s); the Makefile
gained the new Python test. `make test` 95,975 C checks and 179 Python tests,
0 failures (three Python tests more than `0d95ad2`: `test_manuscript_series`);
`make analyze` 0 warnings across 87 source files; `make sanitize` 95,975, 0,
0 diagnostics; `make tsan` 48,605, 0, 0 diagnostics; `make dbtest` 18,000, 0.
Every exit status 0 (`build/gap003/gates-*.log`, `gates-exit.txt`). Hosted
verdicts are appended when the runs finish.
