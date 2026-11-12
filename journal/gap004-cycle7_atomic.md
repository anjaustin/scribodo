# GAP-004, cycle 7, and the register: one atomic writer for the app's small files

**Date:** 2026-11-12. **Tree:** `46d65c2` plus the changes recorded below.

## The item

Two records met: the register's duplication row ("Atomic file write": seven
hand-rolled temp-and-rename copies, parent fsync in three, an `EINTR` loop in
one, two with a fixed `.tmp` name) and the interruption matrix's stated limit
(four of those writers shared the save row's shape and were not driven). One
writer answers both.

## The change

`file_text_write_atomic(path, data, n, mode)` in `src/core/file_text.c`:
mkstemp beside the target (`<path>.tmp-XXXXXX`), fchmod, every byte written
with `EINTR` retried, fsync, close, rename over the target, fsync of the
directory; on failure the temp is removed and the target untouched. It carries
the matrix's two points, `atomic.temp_written` and `atomic.renamed`. The eight
call sites now call it: the observer state and rules files, the recent-projects
list, the import witness (`project_atomic_text`), the store marker
(`write_marker`), saved revisions, the author profile and the model settings.
The two fixed `.tmp` names (profile, model settings) are gone, and every one
of these files now gets the strictest copy's durability. `doc_io.c` keeps its
own writer: it revalidates the target's identity and writability between the
write and the rename, which a shared writer cannot do without taking a
callback, and the register row says so.

## Measurement

`test_doc_io` gains `test_file_text_write_atomic` (whole content, requested
mode, an empty write, no temp left, the original kept when the directory is
write-protected): 62 checks, 0 failures. `make crashtest` gains the row: a
child writing through the shared writer dies at each point; the target is
whole (original, then new), the next write succeeds, and a temp is left after
the first death, exactly as for the save row. 126 matrix rows (122 deaths),
1,408 checks, 0 failures, 50 s.

| Point | Target at death | Temps at death | Next write | Temps after |
|---|---|---:|---|---:|
| `atomic.temp_written` | original, whole | 1 | ok, third text | 1 |
| `atomic.renamed` | new, whole | 0 | ok | 0 |

The stray temp after the first death is the same finding as the save row's,
now for the app's small files too (`<file>.tmp-XXXXXX` beside the profile or
the settings); it stays under "Findings awaiting a decision".

## A process error, recorded

The first commit of this change (`9097df3`) was pushed with three of the five
gates failing: `make test`, `make sanitize` and `make tsan` could not link
`test_store_connections`, whose standalone rules compile `store.c` from source
without `core/file_text.c`, and the command that recorded the gates committed
without being conditioned on their result. The link rules (ordinary, ASan via
the reinvoked build, TSan) now name the writer's source. The rerun then found
a second defect the first run's link failure had hidden: the writer allocated
the directory name after the rename, so an allocation failure injected there
(`test_persistence_faults`, 41 checks) left a saved file while reporting a
failure, the one outcome a save must never produce. Every allocation now
happens before the first side effect. The gates below are the run on that
tree, and the hosted runs of `9097df3` are expected to fail at `make test`.

## Hosted verdicts

CI passed on `fd5ba05`, `4a4aec0` and `46d65c2`; Trusted acceptance failed on
each at "Run scripted UI acceptance" with the console still locked
(TECHNICAL-DEBT, observed 2026-11-12), the database and crash steps having
passed. `9097df3` failed CI at `make test` (the link error recorded in
journal/gap004-cycle7_atomic.md); `de82f6d` carries the fix. The acceptance
jobs are rerun once the console is unlocked.

## Gates

`make test` 95,917 C checks and 167 Python tests, 0 failures; `make dbtest`
18,000, 0; `make crashtest` 1,408 checks, 0 failures; `make analyze` 0 warnings
across 88 source files; `make sanitize` 95,917, 0, 0 diagnostics; `make tsan`
48,565, 0, 0 diagnostics. Every exit status 0 (`build/atomic/gates3-*.log`; the
two failed runs are `gates-*.log` and `gates2-*.log`).
