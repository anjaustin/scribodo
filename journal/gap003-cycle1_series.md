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

- `observer_lag` stays at the message count throughout, so the soak measures
  ingestion and indexing backlog, not observer backlog. (Written at the time
  as the fake model's doing; the reruns below show it is the scripts': every
  UI script runs with the observer off.)
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
Every exit status 0 (`build/gap003/gates-*.log`, `gates-exit.txt`). Hosted:
CI and Trusted acceptance passed on `0534adc` (runs 43405278696 and
43405278581).

## The measured runs: restart and soak (2026-11-13, 10:04 to 14:20 UTC)

Four runs were chained on the `0534adc` build, each on its own disposable
database and project, the store sampled every minute
(`build/gap003/runs.log`, the run directories and logs beside it). Two
passed; two failed, and the failures are the cycle's findings.

| Run | Verdict | What it measured |
|---|---|---|
| `restart-fake-3` (three cycles, fake model) | pass | SIGKILL at 1,124 messages mid-cycle; the same database and folder reopened in 0.19 s (10 frames, worst 29.0 ms), recall 0.30 s (worst 18.4 ms), send 0.85 s (47 frames, worst 38.2 ms), 11 of 11 checks, resume wall 2 s; memory settled on its own (cursor 1125 at the last sequence, outbox empty, 1,916 units, 11.8 MB); messages 1,124 before and after the kill, 1,126 after the resume's question and reply. |
| `restart-real-2` (two cycles, real model) | **fail** | SIGKILL at 1,121 messages; the relaunch said "the model needs about 4.2 GB and 0.0 GB is free for it", `wait_ready` failed after its 600 s, the send produced nothing (messages stayed 1,121), memory stopped one sequence short (cursor 1119, last sequence 1120, the question in flight at the kill never ingested), 10 of 11 checks. The finding below. |
| `soak-fake-memory-120` (120 min, fake model, memory on) | pass | 424 cycles, 2,129 of 2,129 checks; 389,420 frames at p50 17.5, p95 18.0, p99 18.75 ms, worst 115.6 ms; app RSS peak 177.6 MB, final 111.7 MB; MariaDB peak 494 MB and Qdrant peak 887 MB by `docker stats` (cache included); the store grew from 1,116 messages, 1,906 units and 4.6 MB to 1,964 messages, 2,754 units, 432 revisions and 49.7 MB (about 106 KB per cycle of a send and a chapter save with its revision), 2,754 points, the cursor at the last sequence and the outbox empty at the end and at every one of the 116 samples; no gap between samples above 90 s. |
| `soak-real-memory-60` (60 min, real model, memory on) | **void** | Memory enable ingested nothing in 17 minutes (units stayed 0 with memory enabled) and `wait_memory` failed; then at 12:43:05 UTC the Mac, on battery at 1%, entered low-power sleep and hibernated until 14:19:42 UTC when it was plugged in (`pmset -g log`: "Entering Sleep state due to 'Low Power Sleep'", "Wake from Hibernate"). The process clock saw 1,107 s of a 6,890 s wall; one measured frame interval was 14,144 ms. Nothing from this run is a measurement. |

The fake soak is the cycle's sustained-use evidence: two hours of the full
cycle with memory on, the store settling after every cycle, the app's RSS
flat (its peak is the import's, the final figure 66 MB under it) and the
services' figures inside their limits. Its worst frame is the one number
against the proposed budgets: 115.6 ms in 389,420 frames, where the proposal
says 50 ms. The report carries only the maximum, so how many frames crossed
50 ms in two hours is unknown; a count above a threshold belongs in
`FRAME_METRICS` before the worst-frame budget is agreed as an absolute or
restated as a rate (open, below).

## Finding: the instance lock outlives a killed owner (product defect, fixed)

`restart-real-2`'s relaunch never got the model. At 07:30 local, four and a
half hours after that app was killed, its two `llama-server` processes (chat
and embedder, started 03:05:41, parent pid 1) were still running, still
listening on both sockets, and `lsof` showed each holding descriptor 6 on
`run/server.lock` and descriptor 9 on `run-embed/server.lock`; a direct
`flock(LOCK_EX | LOCK_NB)` on either file returned `EWOULDBLOCK`. The
supervisor (`src/llm/server.c`) takes the instance lock with `flock` on a
descriptor opened without close-on-exec and then `posix_spawn`s the server
with default inheritance, so the server inherits the descriptor; a flock
belongs to the open file, not the process, and the server's copy keeps the
lock alive after its owner is gone. Every later window then fails
`acquire_instance_lock`, reports "Another Scribodo window is using the model.
Close it to use the model here", and never reaches `kill_orphan` (the pid-file
reaper that exists for exactly this case). The chat model is gone for good,
memory ingestion and indexing stop with the embedder, and the orphans hold
their 4.2 GB and the embedder's memory. Red-team 003 finding 9 added the
lock so a second window would not kill the first one's server; the lock did
that and also, unseen until a SIGKILL under measurement, survived its owner.

For an author: force-quit the app, or let it crash, while the model is
loaded, and no relaunch has a model until the two servers are killed by hand
or the Mac restarts. The restart scenario in the P0 list is there to find
this.

**Fix (`src/llm/server.c`).** The lock descriptor is opened with `O_CLOEXEC`,
and the server is spawned with `POSIX_SPAWN_CLOEXEC_DEFAULT`, so it gets
stdin, stdout and stderr from the file actions and nothing else of the app's:
not the lock, not the database or vector connections, not another server's
log. The lock now dies with its owner, and the next window's
`acquire_instance_lock` succeeds, `kill_orphan` reaps the orphan through the
pid file (SIGTERM, then SIGKILL after 5 s), and a fresh server starts. The
window between the crash and the next launch, in which the orphan still holds
its memory, is unchanged (there is no parent-death signal on macOS); the next
launch recovers.

**Pin (`test_llm: orphan_after_owner_kill`).** The test spawns itself as the
first window (`test_llm --orphan-owner DIR`: a supervisor over a fake server
script named `fake-llama-server.sh`, so `kill_orphan`'s command-line check
applies), waits for the pid file, kills that window with SIGKILL, checks the
server outlived it, then starts a second supervisor on the same run
directory and requires: not `LLM_FAILED`, a new pid in the pid file, the
orphan gone. Against the old `server.c` (`build/gap003/orphan/test_llm-old.log`,
the old object linked in place of the new): four checks fail and the second
window's status is "Another Scribodo window is using the model. Close it to
use the model here." Against the fix (`test_llm-new.log`): 4,708 checks, 0
failures. `instance_lock` (a live second window still refuses) passes on both.

**The harness says why (`src/app/script.c`, `src/app/script.h`, `src/main.c`).**
`wait_ready`'s failure line read "model did not become ready" and nothing
else, which is why the first reading of `restart-real-2` was a memory
question. The script host gained a `model_status` hook and the failure now
carries the supervisor's own status line. Run against the live orphans
before they were killed (`build/gap003/orphan/resume-diagnostic.log`, the
fixed app, a script of `wait_ready` alone):

```
  FAIL   2  wait_ready 1800
             model did not become ready: Another Scribodo window is using the model. Close it to use the model here.
```

The two orphans from the pre-fix app were then terminated by hand (the one
case the fix cannot reach: a lock inherited before it) and both locks probed
free.

## Finding: the soak ran into low-power sleep (tooling, fixed)

The Mac was on battery for the chain. `caffeinate -i` holds off idle sleep,
not the low-power sleep macOS forces at 1%, and a run on battery is in any
case a run on a throttled machine. `tools/power_source.sh` prints one
`POWER source=... charge=...` line and exits 3 on battery unless
`SCRIBODO_SOAK_ON_BATTERY=1`; the baseline, the soak and the restart runner
call it before they create anything, and the soak and restart runners print
it again at the end so a run that lost AC power says so.
`test_power_source` (four tests, `make test`) pins the AC, battery, opt-in and
no-battery readings from saved `pmset -g batt` output. A sleep inside a run
also shows in the store samples (`STORE_GROWTH ... at=` jumps) and in a
process wall far short of the clock; both are in this run's log.

## The reruns

Both real-model runs again, on the fixed build (the tree is `0534adc` plus
this fix, uncommitted at run time, which is why the runner's `tree=` line
says `0534adc`), the machine idle and on AC power (18% charging at the
start, 34% at the end), the orphans gone, `build/gap003/reruns.log`:

| Run | Verdict | What it measured |
|---|---|---|
| `restart-real-2b` (two cycles, real model) | pass | SIGKILL at 1,120 messages mid-cycle; the relaunch took the lock, reaped the orphans and had the model ready (`wait_ready` passed), reopened the chat in 0.18 s (10 frames, worst 20.1 ms), recall 0.29 s (worst 18.3 ms), the send 39.6 s (2,197 frames, p99 22.5 ms, worst 38.4 ms: 1,082 prompt tokens, 1.3 s to first token, 985 tokens at 25.9 per second), 11 of 11 checks, resume wall 42 s; memory settled on its own (cursor 1121 at the last sequence, outbox empty); messages 1,120 before and after the kill, 1,122 after the resume's question and reply. |
| `soak-real-memory-60b` (real model, memory on) | pass | 35 cycles in 2,447 s (the run sizes itself by the assumed 90 s send; the cycles averaged 58 s), 184 of 184 checks; 121,725 frames at p50 18.0, p95 30.25, p99 37.0 ms, worst 87.8 ms, streaming included; app RSS peak 117.9 MB, final 68.3 MB; MariaDB peak 504 MB, Qdrant 877 MB by `docker stats`; the store from 1,116 messages to 1,186, units 1,198 and points 1,198, 43 revisions, 12.5 MB, the cursor at the last sequence and the outbox empty at the end and no gap between its 42 samples; 35 sends with prompts of 3,138 to 13,179 tokens (median 9,217), time to first token 3.7 to 20.5 s (median 12.0; 628 to 853 prompt tokens per second), 82 to 2,048 tokens generated at 21.7 to 25.5 per second, 15.3 to 112.9 s a send. |

The restart's own numbers are the fix's end-to-end measurement: the first
real-model relaunch failed `wait_ready` after 600 s; this one passed it and
settled memory in a 42 s resume. Three things the reruns show that the
earlier runs could not:

- **With memory on, the prompt is the context.** The baseline series' sends
  (memory off) carried 1,854 to 1,975 prompt tokens and reached the first
  token in 2.3 to 2.5 s. The soak's sends, memory on and a 32K context,
  carried a median 9,217 tokens and reached the first token in a median
  12.0 s, 20.5 s at 13,179 tokens; the rate is the same 630 to 850 prompt
  tokens per second. The proposed "4 s to first token at about 2,000 prompt
  tokens" is a budget for memory off. Memory on needs its own row, stated
  per prompt size or as a cap on the layout's prompt tokens; the author's
  decision.
- **The relaunch after a crash sizes its context while the orphan still
  holds memory.** The resumed app printed "the model needs about 4.2 GB and
  0.2 GB is free for it. It's starting with a 8K context anyway" and ran its
  server with an 8K slot, where the soak's app, launched clean, ran 32K.
  `main.c` computes the memory fit (`memfit_context(sysmem_available(), ...)`)
  before `llm_server_start`, and the orphan is reaped inside the start; the
  fit counts the orphan's 4.2 GB as taken. The relaunch works, with the
  smaller context until the author applies the model settings again. Open,
  below; the next step.
- **The observer is off in every UI script** (`script.h`: scripts start with
  it off; none of the four scripts turns it on), so `observer_lag` equal to
  the message count is the scripts' doing, not the fake model's as the
  smoke's observation above said. Observer backlog under load is unmeasured
  by every run here; a soak with `observer on` is the measurement.

One more difference between the fake and real runs, explained by the code:
the fake soak made 1.70 units per message and the real runs 1.01. Units are
split at `MEMORY_UNIT_TOKENS` by the tokenizer in use (`memory_store.c`,
`memory_job.c`), and the fake backend's tokenizer is not LFM3's; the fake
soak's unit and point counts overstate what the real model's store holds for
the same turns. Growth per cycle is in the same range on both (70 to 106 KB
of data and index).

## What stays open after this cycle

- **The launch-time fit reads a free-memory figure that varies run to run.**
  With no orphan anywhere, the first app of `restart-real-2c` (launched
  minutes after the gates finished) read 2.6 GB free and ran an 8K slot,
  where the soak's clean launch an hour earlier ran 32K; the context an
  author gets depends on what `sysmem_available` sees at that moment. What
  it counts (free, inactive, purgeable, the containers' cache) is the next
  measurement before the fit is tuned.
- **A time-to-first-token budget for memory on**, per prompt size or as a cap
  on the layout's prompt tokens: the author's decision, with the soak's 35
  sends as the evidence.
- **Observer backlog under load** is unmeasured: every UI script runs with
  the observer off. A soak with `observer on` is the measurement.
- **Frames above the budget are not counted.** `FRAME_METRICS` reports p50,
  p95, p99 and the maximum; the two-hour soak's 115.6 ms maximum says the
  proposed 50 ms worst-frame budget was crossed at least once in 389,420
  frames and no more. A `count above N ms` field (the histogram already holds
  it) is the next tooling step, and the budget is then agreed as an absolute
  or restated as a rate per hour; the parsers (`tools/manuscript_series.py`,
  `tests/test_manuscript_series.py`) anchor on the current field order and
  change with it.
- **The orphan's memory between a crash and the next launch** stays held;
  there is no parent-death signal on macOS. Recovery is at the next launch.
- **Two other spawn sites inherit descriptors** (`src/app/service_supervisor.c`
  for `tools/services.sh up`, `src/app/backup.c` for the backup pipeline);
  both are short-lived tools, neither takes a lock, and neither is changed
  here. Recorded in TECHNICAL-DEBT.
- **Housekeeping** (the leftover test databases and collection) is unchanged.

## The next step, done: reap before the fit

`llm_server_reap_orphan(cfg)` (`src/llm/server.c`, public) takes the
instance lock without blocking, reaps the pid file's process if its command
line names `llama-server` and this run directory's socket, removes the pid
file and releases the lock; a live window's lock attempt fails and its
server is left alone. `main.c` calls it for the chat, embedding and (when
the pipeline is on) reranker configurations before `memfit_context` reads
`sysmem_available()`, and says so on stderr. Pin: `test_llm: reap_orphan`
(no pid file: false; a live owner: false and the server untouched; a dead
owner: true, the server gone within the reaper's wait, the pid file removed;
then false again). Gates on this tree (`build/gap003/reap/gates-*.log`):
`make test` 95,994 C checks and 183 Python tests, 0 failures; `make analyze`
0 warnings across 87 source files; `make sanitize` 95,994, 0; `make tsan`
48,624, 0; `make dbtest` 18,000, 0; every exit status 0.

Measured with the restart run again (`build/gap003/restart-real-2c`, real
model, two cycles, idle machine on AC power): killed at 1,121 messages, the
relaunch printed "reaped the chat server of an earlier window" and "reaped
the embedding server of an earlier window", printed no low-memory line, and
its server started with `n_ctx_slot = 32768` where `restart-real-2b`'s
resumed server had `n_ctx_slot = 8192` after "0.2 GB is free for it". The
resume then passed 11 of 11 checks in a 10 s wall (open 0.18 s, recall
0.28 s, a send of 8.1 s: 3,089 prompt tokens, 3.9 s to first token, 104
tokens at 26.6 per second), memory settled, and the record held 1,121
messages before and after the kill and 1,123 after the resume. Hosted: CI
and Trusted acceptance passed on `ee797e0` (runs 43456209062 and
43456209122).

## Gates and verdicts for the fix

On the fixed tree (`build/gap003/orphan/gates-*.log`, `gates-exit.txt`):
`make test` 95,983 C checks and 183 Python tests, 0 failures (eight C checks
and four Python tests more than `0534adc`: `orphan_after_owner_kill` and
`test_power_source`); `make analyze` 0 warnings across 87 source files;
`make sanitize` 95,983, 0, no sanitizer diagnostic; `make tsan` 48,613, 0, no
diagnostic (the new test's spawned first window runs under TSan too);
`make dbtest` 18,000, 0. Every exit status 0. Hosted: CI and Trusted acceptance passed on `e7511b9`
(runs 43453865695 and 43453865553).

## The third step: frames above the budget counted, and the observer under load

`FRAME_METRICS` now carries `over_50ms=N`, the exact number of intervals
above `FRAME_METRICS_BUDGET_MS` (50.0, `src/app/frame_metrics.h`), counted
as the histogram is filled (`test_frame_metrics` pins it: none at 10 ms,
50 or 51 for the 1 to 100 ms ramp, 50.0 itself not counted, 50.1 counted);
the series parser reads the field and prints "not reported" for older logs
(`tests/test_manuscript_series.py`). `tools/history_frame_soak.sh --observer`
inserts `observer on` before the cycles, so `observer_lag` in the store
samples measures the observer's backlog for the first time. Gates on this
tree (`build/gap003/frames/gates-*.log`, `gates-exit.txt`): `make test`
95,994 C checks and 184 Python tests, 0 failures; `make analyze` 0 warnings
across 87 source files; `make sanitize` 95,994, 0; `make tsan` 48,624, 0;
`make dbtest` 18,000, 0; every exit status 0. Hosted: CI and Trusted
acceptance passed on `d0e14f6` (runs 43473198174 and 43473198260).

Two soaks on this build, chained after the gates (`build/gap003/frames/`,
AC power throughout, machine otherwise idle, no `llama-server` alive at the
start):

| Run | Verdict | What was measured |
|---|---|---|
| `soak-fake-memory-30` (30 min, fake model, memory on) | pass | 106 cycles in 1,759 s, 539 of 539 checks; 97,492 frames at p50 17.5, p95 18.0, p99 18.25 ms, worst 70.6 ms, **196 above 50 ms** (0.20 percent, about 6.7 per minute); app RSS peak 177.6 MB (the import), final 152.1 MB; MariaDB and Qdrant unchanged in kind from the two-hour run; the store from 1,116 messages, 1,908 units and 11.4 MB to 1,328 messages, 2,118 units and points, 114 revisions, 21.7 MB; the cursor at the last sequence and the outbox empty at 29 of 31 samples and at the end. |
| `soak-real-memory-observer-60` (real model, memory on, observer on) | pass, and a finding | 35 cycles in 2,379 s (the cycles averaged 68 s), 184 of 184 checks; 123,455 frames at p50 18.25, p95 18.75, p99 33.5 ms, worst 131.5 ms, **17 above 50 ms** (0.014 percent, about 0.4 per minute), streaming included; app RSS peak 117.3 MB, final 93.1 MB; MariaDB peak 523 MB, Qdrant 875 MB by `docker stats`; the store from 1,116 messages and 1,116 units to 1,186 messages, 1,249 units and points, 43 revisions, 12.5 MB; 35 sends with prompts of 1,064 to 4,050 tokens (median 3,371: the launch fit read 2.5 GB free after the fake soak and ran an 8K slot, which bounds the layout), 1.4 to 5.9 s to first token (median 5.0), 431 to 2,048 tokens generated at 24.1 to 25.8 per second, 20.5 to 84.6 s a send. **The observer made no progress: `observer_lag` equalled the message count at all 41 samples and `observer_chats` held no row at the end.** |

Two readings of the store samples that are the design, not a defect: in the
real run the memory cursor was one behind the last sequence at 27 of 41
samples, every one taken during a send (the author's message of the send in
flight; `main.c` pauses the memory job while the chat is busy, and the
cursor caught up before each cycle's check and at the final sample, as in
`soak-real-memory-60b`, whose samples read the same way); and the fake run's
two unsettled samples are the same moment with the fake model's short send.

### Finding: the observer starves under continuous chat

The chat server's log for the real run (`~/Library/Logs/Scribodo/llama-server.log`
from its last `load_model` line, 70 completed requests, 36 cancelled) shows
one pattern 35 times:

1. The send's reply finishes on slot 0.
2. About 1.7 s later the observer, idle for `IDLE_DELAY` (1.5 s), launches
   its first model call for its first job on slot 1 (about 900 prompt
   tokens: the rules and a 3,000-byte chunk of the chat).
3. The soak's hold (10 s) and navigation (about 2 s) pass; the next send
   begins on slot 0, `observer_set_chat_active` sets `cancel`, and the
   server logs `cancel task` for the observer's call 12.0 to 13.1 s after it
   launched, with a few hundred tokens generated.
4. The job is retried from its beginning once the chat is idle again, which
   is step 2 of the next cycle.

So the observer's first job never completed, `observer_chats` was never
written, and the run spent 35 times about 12.5 s of the model's time on
work it threw away. The yield is the design (`src/llm/observer.c`:
"Waiting while you chat…"; a job that is cancelled "is retried once chat is
idle"), and it protects the send's latency on a machine where both share one
server. What it does not do is make progress when the quiet gaps are shorter
than one job, and nothing tells the author that the observer has been
cancelled before finishing anything for an hour. The soak's cadence (a send
every 68 s with a 12 s pause) is harsher than an author's, but a quick
exchange of short questions has the same shape, and the backlog an author
most wants read is the one that accumulates while they chat.

The remedy is a scheduling change, not a prompt change, and it needs one
number first: how long one observer job takes on this model when nothing
interrupts it. That is measured next (the fixture open, the observer on, no
sends), and it decides between a floor on the idle window (the observer
starts only when it can expect to finish) and a starvation guard (after N
cancellations without progress one job runs to completion while the chat
shares the server), each with the send's latency during the overlap measured
before adoption. Until then the finding stands in PRODUCT-GAPS under GAP-003
and in MEMORY-019.

### The uncontended job time, measured

`build/gap003/frames/observer-drain-15.log`: the fixture opened on a fresh
database, `observer on`, no send, 15 minutes, store sampled every minute,
AC power, machine idle (the launch fit read 5.2 GB free and ran a 24K slot).
The observer reads every document before any chat (`observer_job_find`), so
`observer_lag` stayed at 1,116 throughout and the progress is in `records`
and `observer_pieces`: 36 findings written, 32 pieces of `ch01.md` read by
minute six. The chat server completed 30 observer calls in the 12.6 minutes
after the first job started (one `cancel task`, the app's exit), each the
candidate extraction over one chunk: 706 to 878 prompt tokens (1.1 to 1.5 s
of prompt evaluation), 384 to 737 tokens generated at 25.1 to 26.1 per
second, **16.5 to 29.7 s a call, median 26.2 s**, 763 s of model time in
all. One observer job is therefore about half a minute of quiet; the soak's
12 s gaps could never hold one, and at this rate the eight chapters take
about an hour and a half before the chat backlog is touched at all.

The gate's contract settles one of the two remedies: the generative gate is
physically exclusive (one request at the model; `generative_gate_foreground_begin`
cancels the active background ticket), so "one job finishes while the chat
shares the server" is not available without changing that contract. What
remains for the author's decision, each a scheduling change measured before
adoption:

1. **Disclose and prefer chat.** Keep the yield; count consecutive
   cancellations without progress and say so in the observer's status ("the
   observer needs about 30 s of quiet; interrupted N times"). The limit is
   then measured and disclosed, as MEMORY-015 asks; the starvation stays.
2. **Do not start what cannot finish.** Start a call only when the author's
   recent cadence (the gaps between sends) suggests room for one; this stops
   the wasted model time (35 times 12.5 s in the soak) and not the
   starvation.
3. **A bounded hold on the send.** After N cancellations without progress,
   the next send waits for the running observer call to finish, at most the
   call's remaining time (under 30 s). Progress is guaranteed under any
   cadence; one send in a while is slower by up to that much, which the
   author feels, so this needs the author's agreement and the send's
   latency measured with it on.

## What the launch-time fit counts, measured

`tools/sysmem_sample.py` (new, pinned by `test_sysmem_sample`) recomputes
`sysmem_available` from `vm_stat` once a minute beside the resident sizes of
the Docker virtual machine, `llama-server` and the app
(`build/gap003/frames/sysmem-samples.log`). Sampled from 17:30 UTC, idle,
through the hosted acceptance runs for `c715e55` and `58df17d` on this
machine (`make all test dbtest crashtest` and the UI tests against the CI
containers), then three idle minutes:

| Time (UTC) | Available | Anonymous | Compressor | Docker VM RSS | llama-server | App |
|---|---|---|---|---|---|---|
| 17:30 (idle) | 7.54 GB | 5.19 GB | 2.21 GB | 1.59 GB | 0 | 0 |
| 17:38 (dbtest running) | 5.90 | 7.35 | 1.81 | 3.58 | 0 | 0 |
| 17:44 (second run) | 5.09 | 8.29 | 1.66 | 4.19 | 0 | 0 |
| 17:54 (idle again) | 4.59 | 8.58 | 1.51 | 4.43 | 0 | 0 |

The fit's figure fell by 3.0 GB with no model loaded and the app at most
0.18 GB, and the Docker virtual machine's resident memory rose by 2.8 GB over
the same minutes and had not come back after the runner went idle. The VM
(`docker info`: 8.3 GB of the machine's 17.2, the default, no setting pinned)
holds the containers' working set and the Linux page cache for their files
as anonymous memory on the host; MariaDB's and Qdrant's own limits (768 MiB
and 1 GiB each) bound the containers, not the VM. So a launch minutes after
heavy store work (a soak, an import, a `make dbtest`) reads less room than
the same launch an hour later, and gets a smaller context: this is what
`restart-real-2c`'s first app (2.6 GB free after the gates), the observer
soak's app (2.5 GB after the fake soak) and the drain's app (5.2 GB, 24K)
each read. The fit reads the machine correctly; the machine's free memory is
what varies, and the Docker VM is the part that varies most. Whether the VM
returns the memory over a longer idle is in the same log's later samples
(recorded in the follow-up). The remedies are the author's: pin the VM's
memory lower in Docker Desktop (the containers need under 2 GB), or accept
the smaller context after store work, with the fit's low-memory line as the
disclosure it already is. Hosted: CI and Trusted acceptance passed on
`c25cd08` (runs 43478074346 and 43478074276).

The sampler's remaining hour answers whether the VM gives the memory back,
and when (`sysmem-samples.log`, 90 samples):

| Time (UTC) | What ran | Available | Docker VM RSS | llama-server |
|---|---|---|---|---|
| 18:05 | idle, 30 min after the runs | 4.52 GB | 4.61 GB | 0 |
| 18:11 | the replay smoke loads the model | 2.86 | 2.36 | 3.79 |
| 18:17 | smoke over, model gone | 7.06 | 2.10 | 0 |
| 18:23 to 18:35 | two more acceptance runs | 5.43 to 4.90 | 4.16 to 4.62 | 0 |
| 18:41 to 18:59 | the Mars observer run holds the model | 2.53 to 2.75 | 3.93 to 3.38 | 3.71 |

Idle does not return it: thirty minutes after the acceptance runs the VM
still held 4.6 GB. Pressure does: the moment the model took 3.8 GB, the VM
gave back 2.3 GB within a minute (its balloon), and with the model gone the
fit's figure read 7.1 GB, higher than before the runs. So the figure the fit
reads *before* loading the model understates what the machine would have
yielded *to* the model by about the VM's reclaimable cache, 2 GB here, and
that is why a launch after store work sizes an 8K or 24K slot that a launch
an hour later, or a launch that simply tried, would have sized at 32K. The
remedy choices stand as written (pin the VM's cap, or accept the smaller
slot); a third is now on the table for the author: let the fit count the
Docker VM's resident memory above a floor as available, since the VM yields
it under pressure, measured here once and to be measured again before it is
relied on.

**Decided and pinned (2026-11-14).** The author chose the pin: Docker Desktop's
VM is set to 3,072 MiB (`MemoryMiB` in `settings-store.json`; `docker info`
reads 2.84 GiB). The VM's first start at that size exited while the services'
health checks ran and came up clean on relaunch; MariaDB lost the last DDL
before the exit (a dropped test database reappeared; dropped again). At rest
with both containers healthy (MariaDB 194 MiB, Qdrant 510 MiB) the VM holds
2.2 GB resident and `sysmem_available` reads 5.1 GB beside the author's open
desktop; the two hosted runs after the pin passed (43558421428 and
43558421560), with the containers at 193 and 541 MiB inside the VM's cap. The
fit's figure can no longer move by more than the cap.

## The observer's wait mode, built (2026-11-14); measured next

The author's answer to the scheduling remedies: "Can the Observer just go
into a wait mode until the chat settles?" Built as a backoff on the idleness
a job needs: `idle_required` starts at the 1.5 s IDLE_DELAY; each time chat
becomes active while a job is running (the job is cancelled, as before) the
requirement doubles when the job unwinds, to a cap of 120 s; a job that
completes resets it to 1.5 s. While it waits above the base the status reads
"Waiting for the chat to settle (N s)…" instead of "Waiting while you chat…".
Under the measured cadence (a send every 68 s with about 15 s idle between
replies) the observer would have tried at 1.5, 3, 6 and 12 s and then asked
for 24 s and more, which the gaps never give: four cancelled calls instead
of one every cycle, no model time spent on work the next send discards, and
the send no longer waits on cancelling a background call. `test_llm`
`observer_wait_mode` pins it with the fake backend held inside a job
(`SCRIBODO_FAKE_EXTRACT_MS`, a test hook): one cancellation, 3.0 s required,
the status text, no job for the first 1.5 s of idleness, then the job and the
reset. `observer_idle_required` and `observer_chat_cancels` are exposed for
it. The measurement owed is the real-model soak with the observer on
(`history_frame_soak.sh --observer`): the earlier run's 35 cancelled calls
against the count under wait mode, and the send's time to first token with
the observer on against off. Gates on the tree with it
(`build/observer/cycle3-gates/`): test 95,744 C checks and 192 Python tests,
analyze 0/87, sanitize 95,744, tsan 48,339, dbtest 18,001, every exit 0.
