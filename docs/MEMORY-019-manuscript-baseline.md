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

## What is next in S4

- An hours-long soak (the 34-minute run above is the first point), and a
  soak with the real model's sends.
