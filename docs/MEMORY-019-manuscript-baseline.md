# MEMORY-019 — Manuscript-scale baseline (S4, first measurement)

**2026-11-08. First repeatable whole-manuscript measurement for [MEMORY-015](MEMORY-015-effective-memory-contract.md) slice S4 and GAP-003.**

**Status:** harness implemented and run once on this Mac. These are
baseline numbers for the real application with the fake model on a
disposable store, not an SLA and not a sustained-use result. Nothing in the
product changed for this record.

## What was measured

`tools/manuscript_baseline.sh` builds a legacy project from the S2 fixture
(`tests/seed_manuscript.py`: the 1,116-turn, 100,097-word chat as a
`.scribodo` chat log, plus eight chapter documents totaling 95,963 words),
creates a throwaway database, and drives the real app through
`tests/ui/store/16_manuscript_baseline.scribo` under `caffeinate -i` with RSS
sampled every second. Each phase is bracketed by `measure_frames`, so its
line reports the phase's wall time and its worst interframe gap. Frame times
include waits and vsync at 60 Hz, so about 17 to 18 ms is the floor.

| Phase | What happens | Wall | Frames | p95 | Worst frame |
|---|---|---:|---:|---:|---:|
| 1 | First open: the legacy project is moved into the store (1,116 messages, 8 documents) | 0.83 s | 3 | | **791 ms** |
| 2 | Open the 1,116-turn chat (bounded 64-message window) | 0.09 s | 5 | 18 ms | 18 ms |
| 3 | Page older five times and back to latest | 0.53 s | 30 | 18 ms | 18 ms |
| 4 | Open a 12,000-word chapter, edit, save, revision recorded | 0.18 s | 9 | 32 ms | 32 ms |
| 5 | Send a message on the long chat (fake model) | 0.93 s | 52 | 19 ms | 28 ms |
| 6 | Enable memory: ingest and index all 1,116 turns (fake tokenizer, real SQL and Qdrant) | 56.3 s | 3,181 | 18 ms | 64 ms |
| 7 | Recall query over the whole archive | 0.24 s | 14 | 18 ms | 18 ms |

App RSS: 1.6 MB at launch, 177 MB peak and final, 60 samples over 60 s.
Service RSS is not sampled. Total process wall 60.5 s. All 27 script checks
passed.

## What the numbers say

1. **One UI stall, and it is the import.** Moving a whole manuscript into
   the store on first open blocks the frame for about 0.8 s. Every other
   phase stays at the 60 Hz floor, including paging through history on a
   1,116-turn chat and a Recall query over it. The import is a one-time,
   deliberate transition, but it is the only place in this workload where the
   author sees the app freeze, and it is where asynchronous work would show.
2. **The chat window is bounded in practice, not only by design.** Opening
   the long chat took 88 ms and 64 messages, paging took 18 ms frames, and the
   history cache stayed under its 64 MiB cap throughout. MEMORY-003's "do not
   declare host memory bounded end-to-end yet" can now point at a measurement
   rather than an absence of one.
3. **Ingestion runs behind the UI without disturbing it.** 56 s for 1,116
   turns with the fake tokenizer, 3,181 frames at p99 18.5 ms and a single
   64 ms outlier. For comparison, the reach harness measured 99 s for the same
   archive with the real tokenizer and embedder (MEMORY-017), so a real
   manuscript's first enablement is on the order of two minutes of background
   work per 100K words on this Mac.
4. **Memory is modest.** 177 MB for the app with a 100K-word project open,
   fake model, after import, paging, editing, ingestion and recall.

## What this does not establish

- Not an SLA: one run, one machine, fake model, 60 Hz frame floor.
- Not sustained use: one minute, not hours; no restart, no backlog pressure,
  no service stalls.
- Not input latency: interframe time includes waits and vsync.
- Service memory (MariaDB, Qdrant, the model servers) is not sampled.
- The fake tokenizer makes phase 6 a host-path measurement, not a real
  ingestion rate; MEMORY-017 has the real one.

## Remediation of phase 1, measured

`tools/import_probe.c` timed the import itself three times on the same
project: 0.76 to 0.78 s, 2,270 statements for 1,126 inserts in one
transaction, so about two round trips per message (the row insert and its
durable memory intent). The stall was the import running on the frame thread.

The import now runs on an owned worker (`src/app/import_job.c`). `store_import`
already elects one importer with file locks and makes every other thread see
the project as paused, so moving it off the frame thread changes nothing in
its guarantees; the observer joins the project only when the import has
finished, as before, and the outcome is applied by `import_poll` on the
frame thread. The same baseline, same fixture, same machine:

| Phase | Wall before | Worst frame before | Wall after | Worst frame after |
|---|---:|---:|---:|---:|
| 1, first open and import | 0.83 s | **791 ms** | 0.88 s | **18.2 ms** |
| 6, memory enable and ingest | 56.3 s | 64 ms | 53.9 s | 18.8 ms |

All other phases unchanged within the 60 Hz floor; 27 of 27 checks; app RSS
176 MB. Service containers were sampled for the first time: MariaDB 514 MB
and Qdrant 463 MB peak (their resident set as reported by `docker stats`,
including buffer pool and index memory, not attributable to this project
alone). The store UI scripts that open a freshly seeded legacy project now
wait for the import to finish before using it, which is also what an author
sees: the status line says the project is being moved, and the Explorer
shows it paused for about a second.

Batching the inserts would shorten the background 0.8 s but no longer buys
the author anything on this workload; it is noted, not done.

## Real model, and a 34-minute soak

**Real model** (`tools/manuscript_baseline.sh --real`, pinned LFM3 BF16 and
Qwen3 embedder started by the app's own supervisor, 28 of 28 checks):

| Phase | Wall | p99 frame | Worst frame |
|---|---:|---:|---:|
| 1 import (background) | 0.95 s | 17 ms | 17 ms |
| 2 open chat | 0.09 s | 17 ms | 17 ms |
| 3 page five times | 0.51 s | 17 ms | 17 ms |
| 4 chapter edit and save | 0.18 s | 32 ms | 32 ms |
| 5 send on the 1,116-turn chat, real reply | **84.1 s** | 24 ms | 43 ms |
| 6 enable memory: ingest and index 1,116 turns with the real tokenizer | **111.4 s** | 18 ms | 25 ms |
| 7 recall query | 0.25 s | 18 ms | 18 ms |

Phase 5 is the first measured end-to-end reply at manuscript scale on this
Mac: 84 s from Enter to a complete reply against the 64-message window, with
the UI at the frame floor throughout. Phase 6 matches the reach harness's 99 s
for the same archive. App RSS peaked at 117 MB (lower than with the fake
model, whose replies are held differently); MariaDB 575 MB and Qdrant 484 MB.

**Soak** (`tools/history_frame_soak.sh --manuscript --minutes 60`): the
runner's cycle sizing under-counted the manuscript cycle's own work, so the
run lasted 2,042 s, 34 minutes, not 60; the sizing is corrected below and the
number reported is the one measured. 120 cycles of paging, chapter edit and
save, and a fake-model send; 605 checks passed; 116,341 frames at p50
17.75 ms, p95 18.25 ms, p99 18.5 ms, worst 68 ms. App RSS 173 MB peak and
148 MB final: no growth over the run. MariaDB 583 MB peak, 575 MB final;
Qdrant 471 MB peak, 464 MB final. Not hours yet, and still the fake model
for the sends; the first sustained measurement at this scale shows no drift.

## The baseline as a series (2026-11-13)

GAP-003's fifth working item asks for budgets and repeatability, and a budget
has to be read against how far repeated runs sit apart. `tools/manuscript_series.sh`
runs the baseline N times, each on a fresh disposable database and project,
and `tools/manuscript_series.py` tabulates the spread; the tree is `0d95ad2`,
the machine idle, the hosted runner quiet. Columns are minimum / median /
maximum over the runs (`build/gap003/series-fake-5/series.md`,
`series-real-3/series.md`, every `run-N.log` kept).

**Fake model, five runs, 28 of 28 checks each:**

| Phase | Wall s | p99 ms | Worst frame ms |
|---|---:|---:|---:|
| import (background) | 0.92 / 0.93 / 0.93 | 18.0 / 18.3 / 18.3 | 18.0 / 18.3 / 18.3 |
| open chat | 0.10 / 0.10 / 0.11 | 18.2 / 18.2 / 18.3 | 18.2 / 18.2 / 18.3 |
| page five times and back | 0.62 / 0.63 / 0.63 | 18.3 / 18.3 / 20.8 | 18.3 / 18.3 / 20.8 |
| chapter edit and save | 0.21 / 0.22 / 0.23 | 39.0 / 43.3 / 48.0 | 39.0 / 43.3 / 48.0 |
| send (fake reply) | 0.97 / 0.99 / 1.00 | 39.9 / 41.3 / 45.3 | 39.9 / 41.3 / 45.3 |
| enable memory, 1,116 turns, fake tokenizer | 53.97 / 54.02 / 54.26 | 18.3 / 18.3 / 18.5 | 18.3 / 18.3 / 20.1 |
| recall | 0.28 / 0.28 / 0.30 | 19.3 / 19.4 / 19.6 | 19.3 / 19.4 / 19.6 |

App peak RSS 172 / 173 / 173 MB; process wall 58 / 59 / 60 s.

**Real model, three runs, 28 of 28 checks each** (pinned LFM3 BF16 and the
Qwen3 embedder, started by the app's supervisor):

| Phase | Wall s | p99 ms | Worst frame ms |
|---|---:|---:|---:|
| import (background) | 1.00 / 1.00 / 1.07 | 18.1 / 18.1 / 23.4 | 18.1 / 18.1 / 23.4 |
| open chat | 0.10 / 0.11 / 0.11 | 18.1 / 18.1 / 18.2 | 18.1 / 18.1 / 18.2 |
| page five times and back | 0.62 / 0.63 / 0.64 | 18.2 / 18.2 / 18.3 | 18.2 / 18.2 / 18.3 |
| chapter edit and save | 0.20 / 0.22 / 0.22 | 30.4 / 44.8 / 47.8 | 30.4 / 44.8 / 47.8 |
| send (real reply, streamed) | **5.67 / 30.48 / 87.50** | 25.5 / 32.5 / 35.5 | 36.4 / 38.5 / 41.4 |
| enable memory, 1,116 turns, real tokenizer | 118.32 / 118.85 / 119.13 | 18.3 / 18.3 / 18.5 | 22.7 / 24.8 / 26.1 |
| recall | 0.30 / 0.30 / 0.30 | 18.3 / 18.3 / 18.8 | 18.3 / 18.3 / 18.8 |

App peak RSS 106 / 107 / 138 MB; process wall 128 / 154 / 212 s.

What the series says:

1. **Every phase but the send is repeatable to a few percent.** Across the
   eight runs the import, chat open, paging, save, memory enable and recall
   walls sit within 3% of their medians, and the frame figures within a
   frame interval, except the save and the fake send, whose worst frame moves
   between 39 and 48 ms (ten and fifty-five frames respectively; one slow
   frame is the whole p99). These are the phases a budget can be stated for
   directly.
2. **The send's wall is the reply's length, nothing else.** The model
   server's log for the three real sends: prompt evaluation 2.29, 2.45 and
   2.48 s for 1,854, 1,975 and 1,975 tokens (time to first token, within 0.2 s
   of each other), then 41.5, 41.1 and 41.1 ms per generated token (24.1 to
   24.4 tokens per second), for 2,048, 676 and 71 tokens. The first reply
   ran to the request's cap of 2,048 tokens (the chat's sampling profile,
   `max_reply` in chat.c; the recorded requests carry `max_tokens` 2048),
   which is why 87.5 s is the maximum a send can take at this prompt size on
   this Mac. A send budget is therefore
   a budget on time to first token and on tokens per second, with the wall
   bounded by the cap; the UI stays under 42 ms worst frame while a reply
   streams for 85 s.
3. **Memory enable with the real tokenizer costs 0.106 s per turn** (118.3 to
   119.1 s for 1,116), 2.2 times the fake-tokenizer path (54 s), both run
   behind the UI at the frame floor. A real 100K-word chat archive is two
   minutes of background work on first enablement, as MEMORY-017 and the
   first record said.
4. **Service memory, read properly.** `docker stats` reports the container's
   cgroup usage, which includes the page cache. At the end of the series
   Qdrant's cgroup held 890 MB, of which 379 MB anonymous and 469 MB file
   cache (the process's RSS 729 MB: 362 MB anonymous, 368 MB file-mapped
   segments); MariaDB's 491 MB was 408 MB anonymous and 64 MB cache. The
   containers' limits are 1 GiB and 768 MiB. The 2026-11-08 figures (463 and
   514 MB) were the same measure on a younger container; the growth since is
   cache, reclaimable, not a leak claim either way. The soak below samples
   the store's own size and backlog instead of inferring them from RSS.

### Budgets (proposed 2026-11-13, agreed by the author 2026-11-14)

Stated at about twice the series' median where the series is tight, and as
the measured quantity where the wall is not the right unit. Each is a budget
on this Mac for the 100K-word fixture, to be read against the series above,
not an SLA.

| Quantity | Measured (series) | Proposed budget |
|---|---|---|
| Worst interframe gap, any phase | 48.0 ms (save), 45.3 ms (fake send), 41.4 ms (real send) | 50 ms (three frame intervals) |
| p99 interframe gap outside a streaming send and the save | 19.6 ms | 25 ms |
| Open the 1,116-turn chat | 0.10 to 0.11 s | 0.25 s |
| Page older five times and back | 0.62 to 0.64 s | 1.0 s |
| Chapter edit, save, revision recorded | 0.20 to 0.23 s | 0.5 s |
| Recall over the archive | 0.28 to 0.30 s | 1.0 s |
| Import on first open (background) | 0.92 to 1.07 s for 1,116 messages | 2 s |
| Memory enable, real tokenizer | 0.106 s per turn | 0.2 s per turn (223 s for the fixture) |
| Send: time to first token at about 2,000 prompt tokens | 2.29 to 2.48 s | 4 s |
| Send: generation rate | 24.1 to 24.4 tokens per second | at least 20 tokens per second |
| Send: wall | 5.7 to 87.5 s, reply-length bound | 4 s + reply tokens / 20 (106 s at the cap) |
| App peak RSS | 106 to 173 MB | 256 MB |
| MariaDB and Qdrant anonymous memory | 408 and 379 MB | 512 MB each, under the containers' 768 MiB and 1 GiB |

The budgets that bind today are the worst frame (48 of 50 ms on the save)
and the Qdrant anonymous memory if a soak grows it. The author's agreement
turns the column into the acceptance line; until then it is a proposal read
off the series. The two-hour soak below adds one reading against the first
row: a worst frame of 115.6 ms in 389,420 frames, with no count of how many
crossed 50 ms (that report carried the maximum only). Since 2026-11-13
`FRAME_METRICS` counts them (`over_50ms=N`): the 30-minute fake-model soak
crossed 50 ms 196 times in 97,492 frames (0.20 percent, about 6.7 per
minute, worst 70.6 ms) and the 40-minute real-model soak 17 times in 123,455
(0.014 percent, about 0.4 per minute, worst 131.5 ms, streaming included).
The row is agreed as a rate, the author having asked for the line to be set
for best performance (2026-11-14): **at most 0.25 percent of a soak's frames
above 50 ms, and none above 150 ms**, which both soaks pass (0.20 and 0.014
percent; worst 70.6 and 131.5 ms) and which a regression to the two-hour
soak's unmeasured count would be read against. The other rows are adopted as
proposed; the table is the acceptance line from this date.

## Soak and restart (2026-11-13)

The soak and restart tooling (TESTING.md: `tools/history_frame_soak.sh
--manuscript --memory`, `tools/manuscript_restart.sh`,
`tools/store_growth_sample.sh`) ran as a chain on the `0534adc` build, each
run on a disposable database and project, the store sampled every minute.
The journal (`journal/gap003-cycle1_series.md`) has the per-run figures; the
findings:

- **Two hours of the full cycle with memory on, fake model: steady.** 424
  cycles (page, chapter edit and save, send; every send ingested and
  indexed), 2,129 of 2,129 checks. 389,420 frames at p50 17.5, p95 18.0, p99
  18.75 ms and a worst of 115.6 ms. App RSS peak 177.6 MB (the import), final
  111.7 MB. The store grew from 1,116 messages, 1,906 units and 4.6 MB to
  1,964 messages, 2,754 units, 432 revisions and 49.7 MB (about 106 KB per
  cycle), the memory cursor at the last sequence and the outbox empty at
  every one of 116 samples. MariaDB peaked at 494 MB and Qdrant at 887 MB by
  `docker stats` (cache included), inside their limits. The fake model
  reviews nothing, so observer backlog is not measured by this run.
- **Restart with the fake model: the store is the truth.** Killed with
  SIGKILL mid-cycle at 1,124 messages, the same database and folder reopened
  in 0.19 s (worst frame 29.0 ms), recall in 0.30 s, a send in 0.85 s, memory
  settled on its own, and the message count was the kill's plus the
  resume's two. Three cycles before the kill; 11 of 11 checks.
- **Restart with the real model found a product defect, now fixed.** The
  relaunch never got the model: the supervisor's instance lock (an `flock`)
  was inherited by the killed app's two `llama-server` processes and
  outlived it, so every later window reported "Another Scribodo window is
  using the model" and the orphans kept their 4.2 GB. An author who
  force-quits or crashes with the model loaded had no model until the servers
  were killed by hand. The lock descriptor is now close-on-exec and the
  server is spawned with a clean descriptor table; the next launch reaps the
  orphan as designed. Pinned by `test_llm: orphan_after_owner_kill` (fails
  against the old code with that status line). `wait_ready` in the UI
  scripts now reports the supervisor's status when it times out.
- **The real soak was void:** it started against the same held lock (memory
  ingested nothing) and the Mac, on battery at 1%, hibernated for 97 minutes
  in the middle of it. The long-run tools now refuse to start on battery
  (`tools/power_source.sh`) and log the power source at both ends.

Both real-model runs again on the fixed build, idle machine, AC power
(`build/gap003/restart-real-2b`, `soak-real-memory-60b.log`):

- **Restart with the real model, fixed: pass.** Killed at 1,120 messages,
  the relaunch took the lock, reaped the orphans, had the model ready,
  reopened the chat in 0.18 s, recalled in 0.29 s, sent (1.3 s to first
  token at 1,082 prompt tokens, 985 tokens at 25.9 per second) and settled
  memory on its own; 1,120 messages before and after the kill, 1,122 after
  the resume. One thing the relaunch does worse than a clean launch: it
  sized its context with the orphan's 4.2 GB still counted as taken and ran
  an 8K slot where a clean launch ran 32K, because the fit ran before the
  orphan was reaped. Fixed the same day: the app reaps a dead window's
  servers (`llm_server_reap_orphan`, pinned by `test_llm: reap_orphan`)
  before the fit; the restart run again showed the relaunch reaping both
  servers, no low-memory line, and a 32K slot.
- **A real-model soak with memory on: steady, and the prompt is the
  context.** 35 cycles, 184 of 184 checks, 121,725 frames at p50 18.0, p95
  30.25, p99 37.0 ms and a worst of 87.8 ms with the streaming sends
  included; app RSS peak 117.9 MB; the store from 1,116 to 1,186 messages
  and 12.5 MB, the cursor caught up before every cycle's check and at the
  final sample (during a send it waits one message behind: the memory job
  pauses while the chat is busy, by design). The 35 sends carried 3,138 to 13,179
  prompt tokens (median 9,217) and reached the first token in 3.7 to 20.5 s
  (median 12.0) at 630 to 850 prompt tokens per second, then generated 82 to
  2,048 tokens at 21.7 to 25.5 per second. The series' 2.3 to 2.5 s to first
  token was memory off at about 2,000 tokens; memory on needs its own budget
  row, per prompt size or as a cap on the layout's prompt tokens.
- **Frames above the budget, counted (2026-11-13, the third step).** With
  `FRAME_METRICS` counting intervals above 50 ms, a 30-minute fake-model soak
  with memory on (106 cycles, 539 of 539 checks) crossed it 196 times in
  97,492 frames (0.20 percent, worst 70.6 ms) and a 40-minute real-model
  soak with memory and the observer on (35 cycles, 184 of 184) 17 times in
  123,455 (0.014 percent, worst 131.5 ms, streaming included); app RSS peaks
  177.6 and 117.3 MB, MariaDB 523 MB and Qdrant 875 MB at most, the store
  settled between sends and at the end of both. The real run's 35 sends,
  on an 8K slot (the launch fit read 2.5 GB free after the fake soak),
  carried 1,064 to 4,050 prompt tokens and reached the first token in 1.4
  to 5.9 s (median 5.0), then generated 431 to 2,048 tokens at 24.1 to 25.8
  per second.
- **The observer makes no progress under continuous chat.** Measured with
  `--observer` on the real model (journal, "the observer starves under
  continuous chat"): a send every 68 s with a 12 s pause cancelled the
  observer's first model call 35 times out of 35, 12.0 to 13.1 s after it
  started, and the 1,116-turn backlog did not move in 40 minutes; the
  design retries a cancelled job from its beginning once the chat is idle.
  Uncontended, one observer call (the extraction over a 3,000-byte chunk) is
  16.5 to 29.7 s on this model, median 26.2 (30 calls, `observer-drain-15`),
  and the observer reads the eight chapters (about an hour and a half) before
  the chat. The gate is exclusive by contract, so the remedy is one of: a
  disclosed count of interruptions, not starting a call the cadence cannot
  hold, or a bounded hold on the send after repeated starvation (the only one
  that progresses under continuous chat, at up to 30 s on one send); the
  author's decision, measured before adoption (journal).

## What is next in S4

- ~~The author's agreement on the budgets~~ Agreed 2026-11-14: the table
  above, the worst-frame row as a rate (0.25 percent above 50 ms, none above
  150 ms).
- The context after store work: `sysmem_available` reads the machine
  correctly, and what moves it is the Docker virtual machine, whose resident
  memory rose from 1.6 to 4.4 GB over two acceptance runs and did not come
  back in the idle minutes after (`tools/sysmem_sample.py`, journal "what
  the launch-time fit counts"); a launch minutes after a soak or `make
  dbtest` gets a smaller context than one an hour later. The author's
  remedy: pin the VM's memory lower in Docker Desktop (default 8.3 GB of
  17.2; the containers need under 2 GB), accept the smaller context with
  the low-memory line as its disclosure, or let the fit count the VM's
  memory above a floor as available: the VM gave back 2.3 GB within a
  minute of the model loading (its balloon) while thirty idle minutes
  returned nothing, so the pre-load figure understates what the model would
  get by about that much (journal, the sampler's last hour). **Pinned
  2026-11-14 at the author's direction: 3,072 MiB** (`MemoryMiB` in Docker
  Desktop's `settings-store.json`; `docker info` reads 2.84 GiB). The VM's
  first start at that size exited unexpectedly while the services' health
  checks ran and came up clean on relaunch; MariaDB lost the last DDL before
  the exit (a dropped test database reappeared and was dropped again). At
  rest with both containers healthy (MariaDB 194 MiB, Qdrant 510 MiB) the VM
  holds 2.2 GB resident and `sysmem_available` reads 5.1 GB with the rest of
  the author's desktop open; the launch-time fit can no longer be moved by
  more than the VM's cap, which is the point.
- A time-to-first-token row for memory on (the soak's 35 sends: 3.7 to 20.5 s
  at 3,138 to 13,179 prompt tokens), the author's decision on its form.
- The observer's scheduling under chat: the author's choice among the three
  remedies, then the send's latency measured with it on.
