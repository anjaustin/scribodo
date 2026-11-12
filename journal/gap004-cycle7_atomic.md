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

## Gates

Recorded from logs under `build/atomic/gates-*.log` (see the commit).
