# GAP-004, cycle 8: stray save temps, kept and offered on load

**Date:** 2026-11-12. **Tree:** `6913d70` plus the changes recorded below.
**Direction:** "Keep the stray temp files and offer them on load."

## The decision

The save row (`journal/gap004-cycle2_save.md`) found that a process dying
after `save.temp_written` leaves `<document>.scribodo-XXXXXX` beside the
document: the author's complete text as of that save, which nothing read or
removed. The matrix recorded three options and took none, because the choice
belonged to the author under the delete-nothing rule. The author chose the
second and third together: keep the files, offer them on load.

## The change

`doc_recover_strays` (`src/core/doc_io.c`, declared in `doc_io.h`) runs from
`editor_load_file` on every document open. Beside the real document (symlinks
resolved, as the save resolves them) it looks for regular files named exactly
`<basename>.scribodo-` plus six mkstemp characters, and moves each to
`<name> (Scribodo recovered <stamp>).<ext>`: the stamp is the temp's
modification time, the time of the interrupted save; when the name is taken a
counter follows. The move never replaces a file (link then unlink, with an
exclusive create and rename where links are not supported). The recovered name
has the shape of the conflict copy the product already makes when a file
changed on disk, so it lists in the Explorer and opens as a document. The
status line after the open reads, for example:

> An interrupted save left a copy of nav.md, kept as "nav (Scribodo recovered
> 2026-11-12 071357).md" (its text differs from the document)

appended after any load warning the open already had. Nothing is deleted: the
move is a rename within the folder, and the bytes are the temp's.

The app's small files (`<file>.tmp-XXXXXX` beside the profile, the settings or
a marker) hold no author prose and are not a document anyone would open; the
decision applies to them as "keep" only, which is what the product already
did. Recorded in the matrix under "Findings decided".

## Measurement

`make crashtest`: the save row now runs the recovery after each death, before
the next write, and checks the recovered file.

| Point | Death | Target at death | Temps at death | Recovered as a document | Temps after | Recovered text |
|---|---|---|---:|---:|---:|---|
| `save.temp_written` | SIGKILL | original, whole | 1 | **1** | **0** | the new text, whole |
| `save.renamed` | SIGKILL | new, whole | 0 | 0 | 0 | nothing to recover |

126 matrix rows, 122 deaths, 1,413 checks (5 more than the previous tree),
0 failures, 66 s (`build/recover/gates-crashtest.log`, `build/crash/matrix.tsv`,
exit 0).

`test_doc_io: recover_strays` (31 checks): the stray is moved and its bytes
intact, the document untouched; four look-alike names (`-short`, `-toolong`, a
hyphen among the six characters, another document's stray) and a directory with
a stray's name are left alone; a second call finds nothing; a second stray with
the same stamp takes `, 2` and the earlier recovered file is not replaced; a
stray with the document's own bytes is reported as the same text; a document
without an extension and one opened through a symlink recover beside the real
file; a missing document recovers nothing.

`tests/ui/files.scribo` (the real app, scripted input): a stray written beside
`nav.md`, the document dropped; the status carries both sentences, the folder
holds one `nav (Scribodo recovered …).md` and no `nav.md.scribodo-` file. 19
expectations, 0 failed (`build/ui-one/files/stderr.log`, exit 0); the first
run of the script failed its last line because `.scribodo-state` in the
fixture folder matched the bare `.scribodo-` substring, and the expectation now
names the document.

## Gates

Tallied as the sum of every `N checks, M failures` line and every `Ran N tests`
line in the log (the same count on the previous tree's log,
`build/relation/gates3-test.log`, gives 95,636 and 166):
`make test` 95,671 C checks and 166 Python tests, 0 failures; `make crashtest`
1,413, 0 (above); `make analyze` 0 warnings across 88 source files;
`make sanitize` 95,671 checks, 0 failures, 0 diagnostics; `make tsan` 48,300
checks, 0 failures, 0 diagnostics; `tests/ui/files.scribo` 19 expectations, 0
failed. Every exit status 0 (`build/recover/gates2-*.log`,
`build/ui-one/files/stderr.log`).

The first analyzer run reported four `unix.Stream` findings in the byte
comparison (`fread` in a loop after a short read); the comparison was
rewritten on file descriptors and every gate rerun on the final tree
(`build/recover/gates-analyze.log` holds the findings; `gates2-*` the reruns).

## What remains

Nothing of the finding. The register's residual-risk entry (process death is
not power loss) stays, and the matrix's limits with it.
