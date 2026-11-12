# Red-team review 001: editor feasibility spike

**Date:** 2026-11-04
**Scope:** `src/core/*`, `src/ui/*`, `src/main.c`, `Makefile`, and how we use `stb_textedit`, raylib, raygui, and Clay
**Result:** 26 findings in total (17 from the author's pass, 9 from the independent reviewer's), all fixed or documented.
**Method:** Two passes. In the first, the author re-read every data path. The second was an independent reviewer who saw only the code, not the author's findings. Each finding was then fixed and checked by a test that fails without the fix. Every test suite also runs under AddressSanitizer and UndefinedBehaviorSanitizer (`make sanitize`).
**Threat model:** A local, single-user writing app. The harm we care about most is **losing or silently changing an author's text**, followed by crashes and memory-safety bugs. A hostile local process is treated as a secondary concern. That covers files planted in the same directory, or a file another program rewrites while it's open.

## Findings and fixes

Severity: **H** means an author's text can be lost or silently changed, **M** means the app crashes or the editor's state is corrupted, and **L** means a robustness or hygiene issue.

### Author's pass

| # | Sev | Finding | Fix | Verified by |
|---|-----|---------|-----|-------------|
| A1 | H | The close guard compared the current time against a timestamp that started at 0. Closing a dirty document in the app's first 5 seconds discarded it without a prompt. | Added an explicit `close_armed` flag (`main.c`). | Code review; logic is now independent of start time |
| A2 | H | Saving overwrote the file even when another program had changed it since it was loaded, losing that program's changes. | `doc_save` compares file identity (dev, inode, size, mtime in ns) and refuses with `DOC_ERR_CHANGED_ON_DISK`. The UI warns, and a second save confirms the overwrite. | `test_doc_io: refuses_external_change`, `test_editor: save_detects_external_change` |
| A3 | H | Saving through a symlink replaced the link with a regular file. The real file (e.g. in a synced folder) kept stale content. | The target is resolved with `realpath` before saving. | `test_doc_io: save_through_symlink` |
| A4 | H | If an undo record couldn't be stored (out of memory), the history no longer matched the buffer, and a later undo would corrupt text. | Recording failure clears the history. `undo_clear` moves to a never-used state ID, so the document still shows as dirty. | `undo_clear` and ID logic reviewed; fuzz checks the ID → text model |
| A5 | H | Undo or redo could fail partway through a group if memory ran out, leaving the text half-applied. | Buffer capacity for the whole group is reserved up front with `tb_reserve`, so a group applies entirely or not at all. | `test_core`, editor fuzz |
| A6 | M | A predictable temp file name (`<file>.scribodo-tmp`) opened with `fopen("wb")`. A symlink planted at that name would redirect our write, and two saves could collide. | `mkstemp` (O_EXCL, unique name) in the same directory. | `test_doc_io: roundtrip_preserves_bytes` (no temp files left) |
| A7 | M | Saving reset file permissions to the umask default, e.g. a 0600 private draft became 0644. | `fchmod` the temp file to the original mode before the rename. | `test_doc_io: save_preserves_mode` |
| A8 | M | A crash right after a save could lose the rename, because the directory entry wasn't flushed. | `fsync` the parent directory after the rename. | Code review |
| A9 | M | Opening a huge or binary file read it all into memory with no limit, risking a hang or running out of memory. Codepoint counts could also exceed stb_textedit's `int` range. | 64 MB limit, checked with `fstat` before reading and enforced while reading. Paste and replacement are capped the same way. Directories and other non-regular files are refused. | `test_doc_io: rejects_non_regular_and_large` |
| A10 | M | Files with mixed CRLF/LF or bare CR were normalized silently, so the first save changed bytes the author never touched. | Mixed and lone-CR endings are detected, and the status bar warns which style the next save will use. | `test_doc_io: load_flags` |
| A11 | M | stb_textedit's internal undo was shrunk to 2 states and 2 characters, a configuration the library isn't tested with. | Restored the library defaults (99/999). We still never send it undo/redo keys. | Fuzz (200k ops, ASan/UBSan) |
| A12 | M | After edits with nothing selected, stb_textedit leaves stale selection endpoints one past the end of the text. The fuzzer flagged this. It's harmless inside stb, but a trap for any code that reads the range (e.g. future observer source references). | Selection is normalized after every action. | `test_editor: fuzz_editor` invariant check |
| A13 | L | Keystrokes typed while the editor was unfocused, or while dragging a splitter, stayed queued and were inserted later. | Queued characters are drained whenever the editor doesn't have focus. | Code review |
| A14 | L | Shrinking the window could squeeze the editor pane to zero width, since pane widths were only clamped while dragging. | Widths are clamped every frame (each side panel ≤ 30% of the window). | Manual resize |
| A15 | L | Clicking the Save button took focus away from the editor. | Clicks in the toolbar keep editor focus. | Code review |
| A16 | L | Unchecked allocations: Clay's memory arena, `editor_create`, and the save temp path. | Checked, with clean failure. | Code review |
| A17 | L | The gap buffer could overflow `size_t` when growing to extreme sizes. | Overflow guards in `ensure_gap` and `tb_set`. | Code review |

### Independent reviewer's pass

The independent reviewer worked from a snapshot taken partway through the author's fixes, and reproduced 7 of its 8 findings with a probe harness built against the real editor. None of the 8 duplicated A1–A17.

| # | Sev | Finding | Fix | Verified by |
|---|-----|---------|-----|-------------|
| R1 | H | **The overwrite arm never expired.** After one "changed on disk" refusal, *any* later save skipped the disk check. The reviewer reproduced this: editing for 30 minutes and then saving silently destroyed a second external change. A reflexive double ⌘S did the same. There was also no reload or diff, so one side's text was always lost. | A refused save now **writes the author's text to a sibling conflict copy**, e.g. `chapter (Scribodo conflict 2026-11-04 071530).md` (never overwriting an existing file), and leaves the disk version untouched. The overwrite arm is bound to the exact disk state that was warned about, cancelled by any edit, and expires after 30 seconds. | `test_editor: overwrite_arm_is_narrow`, `save_detects_external_change` |
| R2 | H | **Write-protected files opened editable and were replaced on save.** `rename` only needs write access to the directory, so a `0444` manuscript was swapped out. | `doc_load` reports `writable` (via `access(W_OK)`), and such files open read-only. `doc_save` independently refuses a write-protected target with `DOC_ERR_READ_ONLY`. | `test_doc_io: write_protected`, `test_editor: write_protected_opens_read_only` |
| R3 | H | **A failed load left an editable "untitled" buffer that could never be saved.** There's no Save As, and drag-and-drop is refused while dirty, so the text could only be discarded. The default sample path was relative to the working directory, which is `/` when launched from Finder. | The editor is read-only until a document with a usable path loads. A new file in a folder we can't write to also opens read-only. The sample is located relative to the executable. | `test_editor: no_document_is_read_only`; launched from `/` and confirmed by screenshot |
| R4 | M | **Opening a FIFO hung the app.** `open(O_RDONLY)` blocks before the regular-file check (A9) can run. | Open with `O_NONBLOCK`, check `S_ISREG`, then switch back to blocking. | `test_doc_io: fifo_does_not_block` (the suite runs under `alarm(20)`, so a regression fails instead of hanging) |
| R5 | M | **Undo jumped past the saved state.** Typing within 2 seconds of a save merged into the group from before the save, so the saved text couldn't be reached by undo. | `undo_seal` on every successful save. | `test_editor: dirty_tracking` |
| R6 | M | **Up arrow from the end of the document went to the start of the current line.** stb_textedit treats the end of the text as an empty extra row at x=0. Writers type at the end constantly. | Vertical movement (Up/Down/PgUp/PgDn) now runs on our own wrap layout, with a remembered target x. | `test_editor: visual_line_motion` |
| R7 | M | **Line commands acted on whole paragraphs.** ⌘⌫ on a wrapped paragraph erased the entire paragraph, ⌘←/→ jumped to paragraph ends, PgUp stepped by paragraph, and clicking past the end of a wrapped row put the caret on the next row. | Line start and end, ⌘⌫, PgUp/PgDn and click/drag hit-testing all use visual rows from our layout. stb_textedit now handles only character and word editing. | `test_editor: visual_line_motion` |
| R8 | L | **At the size limit, typing or pasting over a selection deleted it and inserted nothing,** silently. | Capacity is checked *before* stb deletes the selection, and a status-bar notice explains the refusal. | `test_editor: size_limit_keeps_selection` |
| R9 | L | **Shortcuts followed physical key positions.** On QWERTZ, ⌘Z and ⌘Y swap, so undo and redo trade places; on Dvorak, ⌘S doesn't save. This was listed under Known limitations; the reviewer showed it was worse than described. | `shortcut_pressed()` asks GLFW which letter each key types in the current layout (`glfwGetKeyName`), falling back to the physical position for non-Latin layouts, as macOS does. | Code review; needs a hands-on check with a non-US layout |

The reviewer also checked these and found them clean: the NEON and plain-C UTF-8 paths, gap-buffer growth, the undo text-store merge cases, the state ID ↔ text model, BOM/CRLF counting, the window-close flag, and zero or negative editor rectangles.

## Verification

- `make test`: 3 suites.
  - `test_core`: UTF-8 (including NEON vs. plain-C equivalence on random input), gap buffer vs. a reference array, undo merge rules, word wrap.
  - `test_doc_io`: real-filesystem tests for byte-for-byte round-trips (BOM, CRLF, no trailing newline, empty, 3 MB mixed-script), symlinks, permissions, external modification, size limit, and a failed save in a read-only directory leaving the original intact.
  - `test_editor`: the real editor and its stb_textedit integration, driven headlessly through `tests/raylib_stubs.c`. It covers deterministic behavior tests plus a fuzz test: 200,000 random operations over 40 seeds (typing, every navigation and delete key, paste, cut, replace-range, select-all, random selections, undo, redo, resizes that change the wrap width, and a clock that moves inside and outside the undo merge window). After each operation it checks that the cursor and selection are in range and the layout rows tile the buffer exactly. It also checks that **every content-state ID always maps to identical text**. Revisiting any state through undo or redo must reproduce it exactly. Finally, undoing everything must restore the loaded text and a clean dirty flag.
- **Mutation check.** Each key fix (R1–R6, R8, A2, A3, A12) was reverted one at a time in a scratch copy of the tree, with a clean rebuild each time, and the suite was confirmed to fail. All 10 were caught. The first attempt at R8 was *missed*: the test pasted text larger than the whole limit, which an earlier check refuses before the selection-aware check runs. The test was strengthened and re-verified. (Clean rebuilds matter here: macOS's bundled GNU Make 3.81 compares timestamps at one-second resolution, and the first mutation pass linked stale objects until the harness cleaned between runs.)
- `make sanitize`: the same suites built with `-fsanitize=address,undefined -fno-sanitize-recover=undefined`. A planted heap overflow, built with the same flags, was confirmed to abort, so a clean run is meaningful.
- The app builds with `-Wall -Wextra -Wpedantic -Wshadow` and produces no warnings.

## Known limitations

These are documented rather than fixed in this spike:

- **Saving replaces the file's inode.** Hard links to the manuscript are split, and extended attributes and Finder tags aren't copied. That's acceptable for `.md`/`.txt` drafts. If it matters, revisit with `copyfile(3)` on macOS.
- **Detecting external changes relies on mtime, size and inode.** A same-size rewrite within the filesystem's timestamp resolution would go undetected. A content hash at save time is the next step if this becomes a concern.
- **The undo history has no size limit.** Memory grows with every edit in a session. Long-term history belongs in document revisions (MariaDB, per the SOW), not in memory.
- **Word wrap and word count are recomputed over the whole document on every edit.** That's fine at chapter size; it should become incremental before book-length files.
- **Input-method composition isn't shown** (raylib doesn't expose it), and **backspace deletes one code point**, not a whole accented character. Both are known gaps for non-Latin scripts.
- **The window close guard asks once:** closing again within 5 seconds discards changes. A proper dialog and autosave/recovery come with the project model.
- **There's no reload-from-disk or diff view after a conflict.** Both versions are preserved (the disk file plus the conflict copy), but merging them is manual for now. The same applies to a save that died before its rename: its temp is kept on the next open as `<name> (Scribodo recovered <stamp>).<ext>` (GAP-004 matrix, 2026-11-12).
- **Caret affinity at soft wraps:** the position exactly at a wrap boundary always displays at the start of the next row, so ⌘→ on a wrapped row stops just before the last character (usually the trailing space).
- **The headless editor tests stub out raylib,** so window, font and real keyboard behavior are checked by screenshot and by hand, not by `make test`.
