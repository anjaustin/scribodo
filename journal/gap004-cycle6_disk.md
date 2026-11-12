# GAP-004, cycle 6: the disk row, and the matrix complete

**Date:** 2026-11-12. **Tree:** `5bffb2b` plus the changes recorded below.

## The row

Not a death: the environment fails. A 2 MB RAM disk (`hdiutil attach -nomount
ram://4096`, `diskutil eraseVolume`, no privileges) holds a legacy project
and a document; the driver fills it to the last byte (writes of 64 KiB, then
4 KiB, 512 bytes and one byte until a one-byte write fails with `ENOSPC`),
then saves the document, imports the project, frees the space and imports
again, and finally write-protects the document and saves once more. The
driver skips the row with a matrix line when no RAM disk can be attached.

## Measurement

| Case | Result | Invariant |
|---|---|---|
| Save on the full disk | `DOC_ERR_IO`, "Save failed: No space left on device" | original whole, no temp left |
| Import on the full disk | refused; files stay `STORE_FILES`, the chat's two messages readable from them | files authoritative, intact |
| Import after the space is freed | ok, `STORE_DB`, two messages, witness gone | data once |
| Save to a write-protected document | `DOC_ERR_READ_ONLY`, "… is write-protected" | original whole, no temp left |

With the earlier rows: 124 matrix rows (120 deaths), 1,384 checks, 0
failures, 52 s.

## Finding, repaired

The import on the full disk was refused with "This project is being moved
into the store by another window". Nothing else was running: the owner lock
file could not be created (`open` with `O_CREAT` failed with `ENOSPC`), and
`store_import` and `store_fork` reported a failed `open` and a failed `flock`
with the same sentence. An author on a full disk would have been told to
close another window. The two sites now report a lock file that could not be
created with its cause ("Couldn't create the project's import lock: No space
left on device") and keep the other-window sentence for a held lock.

## The matrix

The six transitions the gap named are measured: migration (95 deaths), save
(2), store write (4), legacy import (12), fork (7) and the disk row (4
cases). Two unfinished-move defects (import and fork, a plain open never
finished a move that died after its marker) and one misleading refusal were
found and repaired; the stray save temp awaits the author's decision. The
limits are in the matrix: process death is not power loss; four more
temp-and-rename writers share the save row's shape and are not driven; one
process at a time. `docs/GAP-004-interruption-matrix.md` is the exit
evidence; `make crashtest` reproduces every row on this Mac and on the
acceptance runner.

## Gates

`make test` 95,885 C checks and 161 Python tests, 0 failures; `make dbtest`
18,000, 0; `make analyze` 0 warnings across 88 source files; `make sanitize`
95,885, 0, 0 diagnostics; `make tsan` 48,549, 0, 0 diagnostics; `make crashtest`
1,384 checks, 0 failures. Every exit status 0 (`build/crash/gates6-*.log`,
`build/crash/crashtest.log`).
