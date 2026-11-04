# Red-team review 006: projects and documents

**Date:** 2026-11-04
**Scope:** commit `a3a75ff`:
- opening and creating projects, recent projects and the welcome screen;
- New document, Import, and Rename with History and source forwarding;
- the name prompt and context menu, and native dialogs;
- the observer's rename hook.

**Method:**
- An **independent reviewer** used UI scripts with the fake model, plus a case-sensitive APFS disk image. It reproduced 10 of its 13 findings.
- **Fixes:** each one has a regression test. A mutation check, with build and binary-change validation, confirmed that breaking each fix fails a test.

**Result:** 13 findings. Two could destroy or misplace author files. All are fixed.

## Findings

| # | Sev | Finding | Fix | Verified by |
|---|-----|---------|-----|-------------|
| 1 | H | **A case-only rename overwrote a different file on a case-sensitive volume.** The "same file" test compared names with `strcasecmp`, so `a.md` → `A.md` skipped both the collision check and `RENAME_EXCL`. On a case-sensitive disk, the other `A.md` was replaced. | "Same file" is decided by inode (`st_dev`/`st_ino`). Otherwise the collision check and `RENAME_EXCL` always apply. | `tools/cstest.sh` (`make cstest`): a temporary case-sensitive image, with both files intact |
| 2 | H | **A rename prompt left open across a project switch renamed a file in the other project.** Drops bypassed the modal state. | Drops are ignored behind a prompt or menu (real and scripted). A project change closes both. The prompt remembers its project and refuses if it changed. | `projects_rt.scribo` |
| 3 | M | **The observer applied a queued rename to whichever project was open next.** It moved B's read state, and A's renamed document was re-read. | Each queued rename carries its project and is applied only to that project's state. The queue is unbounded. | `test_llm: recent_and_renames` |
| 4 | M | **Sources followed renames regardless of time.** A note on a *new* `one.md`, made after `one.md` → `two.md`, opened `two.md`. | Renames are timestamped to the millisecond. A source follows only renames made after its snapshot was taken; the snapshot name encodes that time. | `projects_rt.scribo` |
| 5 | M | **A case-only rename disconnected History.** On a case-insensitive volume, `revisions/One.md` *is* `revisions/one.md`, so the "earlier history" was moved aside. | A case-only History move is a direct rename, with no set-aside. | `projects_rt.scribo` |
| 6 | M | **Failures were reported as plain success.** "History couldn't be moved" and "rename not recorded" were overwritten by "Renamed to …". A new document with an old name inherited that name's leftover History. | The final status lists every failure. New documents set stale History aside. | `projects_rt.scribo` |
| 7 | M | **Chat reply links worked behind the prompt and menu.** Apply opened a review, and Note saved a note. | A mouse block in the input layer, set while a prompt or menu is open, covers every view that reads the mouse directly. The prompt's own field is exempt. | `projects_rt.scribo` |
| 8 | M | **The same leak in the note/canon panel and the editor.** The panel's source link and history lines, and editor clicks, drags and wheel, all acted behind the prompt. | Same block. | `projects_rt.scribo` (the cursor doesn't move) |
| 9 | M | **The context menu stored a list index.** A refresh while it was open renamed the wrong document. | The menu stores the document's name and checks it still exists. | `projects_rt.scribo` |
| 10 | M | **`.markdown` files could be created and imported but were never listed or read.** | Listed by the Explorer and read by the observer. | `projects_rt.scribo` |
| 11 | L | **Esc both cancelled the prompt and moved focus.** | Gated by the modal state. | code review |
| 12 | L | **Unicode case and normalization:**<br>• `é.md` → `É.md`, and retyping an NFD name in NFC, were refused;<br>• the open document wasn't followed when its path differed only in case;<br>• `recent.json` kept a renamed document's old name;<br>• import treated a read error as end of file. | Inode-based same-file tests, for the rename and for the open document. `recent_rename_doc`. `ferror` is checked on import. | `make cstest`; `recent_and_renames` |
| 13 | L | **Recent-projects robustness:**<br>• gone folders kept their places;<br>• a corrupt file was silently replaced;<br>• no fallback when `HOME` is unset;<br>• the state folder wasn't created with its parents;<br>• `~` matched `/Users/name2`. | Gone folders are dropped on save. A corrupt file is kept as `recent.json.corrupt-<time>`. `getpwuid` is used when `HOME` is unset. Parents are created. `~` matches only on a `/` boundary. | `recent_and_renames` |

## Verification

- **`make test`:** 10,005 checks. `make sanitize` and `make tsan` are clean.
- **`make uitest`:** 18 scripts, including the new `projects_rt.scribo` (25 checks).
- **`make cstest`:** new; a case-sensitive volume.
- **`make uitest-real`:** passes.
- **Mutation check:** 13 of 13 caught. Two needed stronger tests:
  - `.markdown` listing: the check for one row became a check for the specific row.
  - Observer per-project renames: the bad rename was undone by pruning before state was saved, so the test now checks that no document is re-read.
  
  A 14th mutation, rename time in seconds, was dropped: whether it shows depends on the clock.

## Remaining risks

- **Renames are tracked by name.** File names are the documents' keys until the MariaDB store gives documents stable ids. That replaces `renames.jsonl` and its time rules (see `STORE-001-design.md`).
- **Import copies on the UI thread.** Fine for manuscripts, slow for very large files.
- **Native dialogs are verified by hand only.** UI scripts answer them with `dialog`; the macOS panels themselves were checked manually.
