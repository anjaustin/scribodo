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

## Second step: the table is complete

The four flags `main.c` read and pushed through `ChatSession` setters
(`SCRIBODO_ANSWER_PIPELINE`, `SCRIBODO_MEMORY_EXPERIMENT`,
`SCRIBODO_MEMORY_BRIEFS`, `SCRIBODO_MEMORY_PRESENTATION`) and
`SCRIBODO_INDEX_TEXT_FILE` (read inside `memory_index_text.c`'s own once-init)
are fields of the same table; `main.c` and `memory_index_text.c` read it.
The combination rules stay at the use site (the pipeline needs the memory
experiment; a presentation needs it too), so behaviour is unchanged by
construction and measured the same way: instrumented build
`build/answer-live-flags2`, run `mars-pipe6` (325 s) against `mars-pipe5`,
same environment.

| | mars-pipe5 | mars-pipe6 |
|---|---|---|
| Composer requests (`request-0..9.json`) | 10 | **10 of 10 byte-identical** |
| Composer replies (`content-0..9.txt`) | 10 | **10 of 10 byte-identical** |
| Trace lines under `SCRIBODO_MEMORY_TRACE=1` | 37 | 37 |

Every experiment flag the product reads is now one of eleven fields in
`src/core/experiments.h`, each with its variable, record and measured default
beside it. The remaining `getenv("SCRIBODO_...")` sites in `src/` (36) are
service, model, secret and script-host configuration, which the module's
header says stay where they are read. `test_core` covers all eleven fields
(read once, exact values only, reload, the path copied, an empty path off).

Gates: `make test` 95,847 checks, 0 failures; `make dbtest` 18,000, 0;
`make analyze` 0 warnings across 87 files; `make sanitize` 0 diagnostics;
`make tsan` 48,511, 0, 0 diagnostics (`build/window/flags2-gates-*.log`).

## What remains of the register entry

Migration 015's column (`memory_units.index_text`), to be decided when the
index-layer question in `journal/answer-cycle3_coref.md` is decided: permanent
with an in-app resolver, or dropped. The register entry's flag list is now
the header file.

## Hosted verdicts

| Commit | CI | Trusted acceptance |
|---|---|---|
| `815e0fd` (step one) | success, run 43126177443 | success, run 43126187394 |
| `8298782` (step two) | success, run 43098799042 | success, run 43098799175 |

The push of `815e0fd` produced CI run 43097549796 and acceptance run
43097549847, which stayed `queued` with no job for three and a half
hours while the next push's runs completed; GitHub refused cancel, re-run and
force-cancel for them with three different reasons. The verdicts above for
`815e0fd` come from a branch pushed at the same SHA (`verify/815e0fd`, deleted
after the runs) and a manual dispatch of the acceptance workflow on it. The
remedy is recorded in TECHNICAL-DEBT under "CI reports after direct pushes".
Logs: `build/window/flags-ci-43126177443.log`,
`build/window/flags-acceptance-43126187394.log`.
