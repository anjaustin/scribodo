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

## What is next in S4

- Make the import asynchronous or progressive so phase 1's 0.8 s stall does
  not block the frame, then re-measure with this script.
- Run the same script with the real model (`SCRIBODO_LLM=real` is not yet
  wired into this runner) to put a real time-to-first-token on phase 5 at
  this scale.
- A longer soak on this project: `tools/history_frame_soak.sh` already
  exists for hours-long frame sampling; pointing it at the manuscript seed
  is the next measurement.
- Sample service RSS alongside the app.
