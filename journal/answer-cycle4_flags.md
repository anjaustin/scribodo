# Answer cycle 4, the P0 list: experiment flags read once

**Date:** 2026-11-11. **Tree:** `74bc6a0` plus the changes recorded below.
**Direction:** "Let's make the P0s P0. Start with the lowest-hanging fruit.
Let's measure, always."

## The item and why it is first

The register's P1 entry (TECHNICAL-DEBT, "experiment flags and columns have no
lifecycle") counted nine experiment flags parsed ad hoc at the point of use in
six files, one of them re-read per ingest step inside a store function, one
documented nowhere. Of the P0 working order this is the smallest unit of work
with an exact measurement: the product's behaviour under the default flags
must not change, and "must not change" here is byte-identity of every request
the composer is sent and every reply it gives on a recorded run.

One correction before the work: the register's remediation written this
morning said "retire" the two concluded flags. The author's standing rule
(MEMORY-015 §5, delete nothing, only archive) keeps a failed experiment's code
in the tree, flag-gated; retiring means a move into the harness, not a
deletion. The entry now says so, and the two flags stay gated.

## The change

`src/core/experiments.{h,c}`: one `Experiments` table read from the
environment on first use (`pthread_once`), immutable afterwards, with
`experiments_reload()` for tests that set the environment between calls. Each
field carries its variable, its journal and its measured default in the header
comment, which is the list the register's rule asks for. The six flags that
were read inside modules now come from it:

| Flag | Was read in | Reads before | Reads after |
|---|---|---:|---:|
| `SCRIBODO_ANSWER_BLOCK` | `chat.c` (every pipeline request) | 1 site | table |
| `SCRIBODO_ANSWER_PARTS` | `answer_pipeline.c` (every pipeline run) | 1 site | table |
| `SCRIBODO_MEMORY_TRACE` | `chat.c` (three sites per turn) | 3 sites | table |
| `SCRIBODO_MEMORY_EXCERPTS` | `context_planner.c` (every plan) | 1 site | table |
| `SCRIBODO_MEMORY_SMALL_UNITS` | `context_planner.c` and `memory_document_store.c` (**every ingest step**) | 2 sites | table |
| `SCRIBODO_SEM_DEBUG` | `semantic.c` (every failed Qdrant call) | 1 site | table |

`getenv` for these names now occurs in one file. The four flags `main.c`
reads once and pushes through `ChatSession` setters, `SCRIBODO_INDEX_TEXT_FILE`
with its own once-table, and the service and script-host variables are
unchanged and outside this step; the register says which remain.

`SCRIBODO_ANSWER_BLOCK` has a README row. `CONTEXT_PLANNER_TEST_SRCS` and
`CORE_SRCS` carry the new file; the three tests that set these variables call
`experiments_reload()` after doing so; `test_core` gains a case for the table
(read once, exact values only, reload).

## Measurement

The product under its default flags must behave identically. The measure is
the second novel's boundary script, run live on an instrumented build of this
tree (`build/answer-live-flags`, run `mars-pipe5`, 327 s) with the same
environment as the previous run on the same tree (`mars-pipe4`:
`SCRIBODO_ANSWER_PIPELINE=verified`, `SCRIBODO_MEMORY_TRACE=1`,
`SCRIBODO_MEMORY_EXCERPTS=0`, seed 42), compared file by file:

| | mars-pipe4 (before) | mars-pipe5 (after) |
|---|---|---|
| Composer requests sent (`request-0..9.json`) | 10 | **10 of 10 byte-identical** |
| Composer replies (`content-0..9.txt`) | 10 | **10 of 10 byte-identical** |
| Trace lines on stderr under `SCRIBODO_MEMORY_TRACE=1` | 37 | 37 |

The flag that governs the request shape (`SCRIBODO_ANSWER_BLOCK`, default) and
the flag that governs the trace (`SCRIBODO_MEMORY_TRACE=1`, set) are both
exercised by this run and both read from the table; the excerpt flag is set to
`0` and read as off, as before. The per-ingest-step read is gone and the
ingested units are the same, since the requests built from them are.

`getenv("SCRIBODO_...")` sites for the six flags in `src/`: 9 before, 1 file
after (`src/core/experiments.c`).

Gates: `make test` 95,844 checks, 0 failures; `make dbtest` 18,000 checks, 0
failures; `make analyze` 0 warnings across 87 files; `make sanitize` 0
diagnostics; `make tsan` 48,511 checks, 0 failures, 0 diagnostics
(`build/window/flags-gates-*.log`). The live run and the comparison are kept
under `build/product-memory-use/mars-pipe5` and `build/window/capture-mars-flags`;
archived with the next evidence tarball.

## What remains of the register entry

Listed in TECHNICAL-DEBT under the entry's status: the four flags `main.c`
reads and pushes through `ChatSession` setters (already read once, not yet in
the table), `SCRIBODO_INDEX_TEXT_FILE` with its own once-table, and the
decision on migration 015's column. The next step of the same kind is to
move the `main.c` four into the table, with the same byte-identity
measurement; the column decision waits on the index-layer decision.
