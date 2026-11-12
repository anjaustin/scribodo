# GAP-004, cycle 2: the document save row

**Date:** 2026-11-11. **Tree:** `bb0a88e` plus the changes recorded below.

## The row

`doc_save` (`src/core/doc_io.c`) writes the document to `<target>.scribodo-XXXXXX`
(mkstemp, mode copied from the target), fsyncs it, closes it, revalidates the
target's identity and writability, renames the temp over the target, and
fsyncs the directory. Two points name the moments between: `save.temp_written`
after the close succeeds and `save.renamed` after the rename. The invariant: the
target holds the previous or the new text whole, never a mix, a truncation or
nothing; and the next process's save succeeds and the target then holds that.

## Measurement

`make crashtest` (`build/crash/crashtest.log`, `build/crash/matrix.tsv`): the
driver saves the original text cleanly, forks a child that saves a longer
replacement under the point, and checks after the death.

| Point | Death | Target at death | Temps at death | Next save | Temps after |
|---|---|---|---:|---|---:|
| `save.temp_written` | SIGKILL | original, whole | 1 | ok, third text whole | **1** |
| `save.renamed` | SIGKILL | new, whole | 0 | ok, third text whole | 0 |

With the migration rows: 97 deaths, 888 checks, 0 failures, 32 s.

## Finding

The row holds, and the one observation is the stray temp: a death between the
complete temp and the rename leaves the author's full text in a 0600 file
beside the document, and nothing in the product reads or removes it. Recorded
in the matrix under "Findings awaiting a decision" with the options; nothing
changed, because the choice (remove, name, or offer) is the author's under the
delete-nothing rule.

## Gates

`make test` 95,885 C checks and 161 Python tests, 0 failures; `make dbtest`
18,000, 0; `make analyze` 0 warnings across 88 source files; `make sanitize`
95,885, 0, 0 diagnostics; `make tsan` 48,549, 0, 0 diagnostics; `make crashtest`
888 checks, 0 failures. Every exit status 0 (`build/crash/gates2-*.log`,
`build/crash/crashtest.log`).
