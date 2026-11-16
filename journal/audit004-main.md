# AUDIT-004 remediation: the main.c stream (F3, F12)

**Date:** 2026-11-15. **Branch:** `remed/main` (worktree `build/wt-main`), from `main` at `ed95681`.
**Scope:** `docs/AUDIT-004-repository-audit.md` F3 (main.c keeps absorbing workflows; `main()` 947 lines) and the
main.c/script-host rows of F12. Every change is a behaviour-preserving refactor except item 5 (the draft write
moved off the frame thread after measurement) and two analyzer-driven lines noted under item 2. The services stream
owns the service-script lookup (`main.c` ~799) and the services-started status (~4083); neither was edited, and the
loop head that holds the latter stays in `main()` byte for byte so its merge stays clean.

## Commits

| Item | Commit | Subject |
|---|---|---|
| 1 | `6468571` | move draft, chat-resave and model-startup workflows out of main.c |
| 5 | `6140f60` | draft writes leave the frame thread |
| 3 | `3aec051` | compile-time guards on App's controller mirrors and ScriptHost |
| 4 | `20c0daf` | the environment is written only before the first thread |
| 2 | `6e5d52b` | split main() into phases and build_layout into panes |
| 6 | `4f508bd` | split the script host's run_expect and script_frame |

Item 5 was measured and committed right after item 1 because it rewrites the module item 1 created.

## Sizes

Function lengths by one counter (from the signature, or the comment block directly above it, to the closing brace;
it reads `main()` at `ed95681` as 959 where the audit read 947, so the audit's figures are kept for the old column).

| | `ed95681` | `4f508bd` |
|---|---|---|
| `src/main.c` lines | 4,703 | 4,912 |
| `main()` | 947 (audit) | 80 |
| `build_layout` | 247 | 32 (panes: `layout_chat` 67, `layout_explorer` 60, `layout_entities` 39) |
| longest in main.c | `main` 947, `build_layout` 247, `finish_prompt` 178 | `finish_prompt` 178, `send_chat` 121, `frame_left_click` 98, `main` 80 |
| `src/app/script.c` lines | 1,026 | 1,085 |
| `run_expect` | 211 (audit: 209) | 12 (groups, longest `expect_editor` 64) |
| `script_frame` | 129 | 23 (`step_waits` 29) |

**main.c grew by 209 lines.** The workflow bodies left (item 1, about -65), but the layout guards (+53), the
designated `ScriptHost` initialiser (+26) and about 30 new function frames with their comments (item 2) added more than
left. The file is now 88 lines short of the register's 5,000-line split trigger. The split makes the next move cheap
(the frame and startup phases take `App *` and could leave the file with `App`), but none of that was in scope here.
`run_line` in `script.c` (289 lines) was not asked for and is unchanged.

## Item 1: workflow bodies out of the composition root

- `src/app/draft_controller.{c,h}`: the unsaved-draft workflow (`draft_tick`, `draft_discard`, `fnv1a`, the four App
  fields) as `DraftController`. main.c keeps a five-line adapter that passes the editor's path, dirty flag, clock and
  a text callback. The hash constant is main.c's `1469598103934665603`, which is not FNV-1a's standard offset basis
  (`14695981039346656037`, a digit short); it is only compared in memory, so it was kept as is and the test pins it.
  Tests: `tests/test_draft_controller.c` (interval, change check, replacement, discard on clean, close and switch,
  text and write failures).
- `chat_store_resave_failed` (`src/app/chat_store.c`): the copy-and-append body of `resave_unsaved_chat`; main.c
  keeps the session guard, status and list refresh. Test: `test_llm` `chat_store_resave_failed` (exactly the marked
  messages, in order; none marked writes nothing; a refused append is reported).
- `src/app/local_models.{c,h}`: `local_models_reranker_present` and `local_models_reap_orphans`, the reranker-missing
  check and orphan reaping that were inline in `main()`; same messages. Test: `test_llm` `local_models_checks`.

## Item 5: drafts on the frame thread, measured

`tools/draft_tick_probe.c` (`make build/draft_tick_probe`) loads a file into the real `Editor` (headless stubs),
changes one character per tick untimed, and times `draft_controller_tick` as main.c calls it, 40 ticks per file.
Inputs: a synthetic 1.2 MB buffer and three novels from `build/manuscripts` (Pride and Prejudice 757 KB, Wuthering
Heights 681 KB, A Princess of Mars 392 KB). The machine was not idle (other streams were building; load average
about 3.5), which is the condition the tail matters in.

Before (the inline write, as at `6468571`), four runs:

| Buffer | median | p90 | worst | worst's write+fsync |
|---|---|---|---|---|
| synthetic 1.2 MB | 4.1-8.2 ms | 9.5-10.6 ms | **54.9 ms** | 53.0 ms |
| Pride and Prejudice | 3.3-3.9 ms | 4.5-4.8 ms | 15.1 ms | 14.0 ms |
| Wuthering Heights | 3.6 ms | 4.7 ms | 7.6 ms | 6.6 ms |
| A Princess of Mars | 3.8 ms | 5.2 ms | 8.3 ms | 7.3 ms |

The snapshot (`editor_text_utf8`) took 0.3-0.5 ms and the hash 0.6-1.4 ms; the rest is `write` plus `fsync` plus the
parent `fsync`. One tick in 160 at 1.2 MB exceeded the 50 ms worst-frame budget (journal MEMORY-019 "Budgets"), so
the write moved.

Change: the frame thread takes the snapshot and its hash; `doc_write_draft` runs unchanged (same atomic temp, write,
`fsync`, previous draft removed, folder `fsync`; it has no crash points) on a joinable thread per write, one at a
time, joined when the frame thread next polls. A discard, a document switch or a close while a write is in flight
removes the previous draft at once and the in-flight one when it finishes; a failed first write leaves the
controller owning nothing; shutdown (`app_destroy`) waits for the write and leaves the draft on disk for recovery, as
before; if no thread can be created the write runs inline as before.

After (`--inline` reproduces the old cost for comparison), three runs:

| Buffer | frame median | frame worst | write median | write worst |
|---|---|---|---|---|
| synthetic 1.2 MB | 1.8 ms | 3.1 ms | 3.0-10.5 ms | 21.2 ms |
| Pride and Prejudice | 1.2 ms | 2.1 ms | 2.0-10.5 ms | 34.0 ms |
| Wuthering Heights | 1.2 ms | 1.4 ms | 2.0-9.9 ms | 38.6 ms |
| A Princess of Mars | 0.6 ms | 1.1 ms | 2.9-6.6 ms | 20.2 ms |

Tests: `test_draft_controller` `in_flight` (discard and switch while writing, one write at a time, exit with a write
in flight leaves a recoverable draft); the test also runs under `make tsan` (added to its list), clean five times
before the commit and in the final gate.

## Item 3: compile-time guards

- `APP_MIRRORS` / `APP_MIRROR_ENDS` in main.c, after `App`: for all 26 mirrored fields of the four unions, offset,
  size and type (`__builtin_types_compatible_p`) against the controller field, and the last mirror must end where the
  controller ends (rounded to its alignment). Tried by editing the headers: a field appended to `ChatController`,
  one inserted in `ProjectSession`, `edit_mode` retyped, `project_generation` made signed and `edit_banner` resized
  each fail the build with the field named. The one change these cannot see is a new field slotted into existing
  interior padding (for example a `bool` after `attach_touched`); it shifts no mirror, so aliasing stays correct.
- `ScriptHost` is built with designated initialisers. `src/app/script_host.c` names all 32 members;
  `SCRIPT_HOST_MEMBERS` and a static assert tie the count to `sizeof(ScriptHost)` (every member is one pointer), so a
  new member fails the build until it is named (tried). In script mode main() refuses to start, exit 2, when
  `script_host_missing` finds a member left NULL. Test: `tests/test_script_host.c` (complete host; each of the 32
  pointer slots detected under a distinct name; the first missing one reported).

## Item 4: setenv before any thread

`src/app/startup_env.{c,h}`: `startup_env_prepare` is the first call in `main()`. It reads `SCRIBODO_SCRIPT`,
`SCRIBODO_SCRIPT_DIR` (the `getenv(...) ? getenv(...) : "."` idiom at four sites plus `script_load`'s, nine reads, is now one read) and
`SCRIBODO_SHOT_DIR` once into a struct, applies the UI-test redirects (`SCRIBODO_MODEL_SETTINGS`,
`SCRIBODO_REMOTE_CONFIG`, `SCRIBODO_PROFILE`, each only when unset, as before), removes `SSLKEYLOGFILE`, and seals the
environment. Any later `startup_env_set`/`startup_env_unset` is refused and reported on stderr. Before, the remote and
profile writes ran after `llm_server_start`, `memory_job_create` and `observer_create`. The readers (`remote_load`,
`profile_load`, `model_settings_load`) run where they did and see the same values. Checks:
`tests/test_startup_env.c` (redirects, a run-supplied value kept, the seal refusing writes) and
`tests/test_env_writes.py` (in `make test`): no `setenv`/`unsetenv`/`putenv` anywhere in `src/` outside
`startup_env.c`, and `startup_env_prepare` precedes everything in `main()` that creates a thread-owning object; a
`setenv` planted in `main()` fails it (tried).

## Item 2: main() and build_layout split

`main()` is the environment, the store, seven startup phases (`app_init_jobs`, `app_open_window`,
`app_start_models`, `app_create_views`, `app_load_recents`, `script_host_setup`, `app_open_initial`; each returns 0
or the exit code and `main()` runs the one `app_destroy`, as each early return did), `install_quit_signals`, the loop
and shutdown. The loop head (the signal check, `script_frame`, the server ticks, the store probe and the
service-supervisor poll) stays in `main()` unchanged; the rest of the frame is `frame_step` over fourteen `frame_*`
functions in the original order, with the cross-frame locals in `MainLoop` and the control rects in `FrameRects`.
`build_layout` calls `layout_toolbar`, `layout_explorer` (with `layout_entities`), `layout_chat`,
`layout_side_panel` and `layout_status_bar` inside the same Clay elements.

Verification beyond the gates: the moved text was extracted and compared line by line against the old function with
`app.`/`&app` normalised to `app->`/`app`; the only differences are the removed `app_destroy` calls, the renamed
loop locals (`host`, `sample`, the six rects, `cursor_shape`, `screenshot_path`, `frame`, `running`) and the new
function frames. Same check for `build_layout`: no old line missing.

Two lines changed for the analyzer, not for the split: once `main()` was small, `clang --analyze` reached
`import_documents` and reported a read after a short read in its copy loop (`unix.Stream`, twice). The loop now stops
on a short read; `ferror` is still checked after it, so a read error still fails the import.

## Item 6: the script host

`run_expect` dispatches to `expect_editor`, `expect_chat`, `expect_memory`, `expect_panels`, `expect_files` and
`expect_exit`; each branch moved verbatim (checked by the same line comparison), each expect name in exactly one
group, and an unmatched or ill-formed expect still reports "malformed expect". `script_frame` is the frame accounting
and the loop over `step_waits`, which checks the waits in the original order; the six condition waits share
`settle()`, and the model, observer and chat waits keep their own steps because they differ (no new batch on a ready
model; an observer timeout without one; the chat wait reports on its own line).

## Gates

Per commit, `make test` and `make analyze` (exit status read, not the printed text):

| After | make test | make analyze |
|---|---|---|
| baseline `ed95681` | 0 | - |
| item 1 | 0 | 0 |
| item 5 | 0 | 0 (the first run was killed at about 16:50 by another stream's `pkill -f "clang --analyze"`, exit 2, log "Terminated: 15"; the rerun, which is the one counted, exited 0) |
| item 3 | 0 | 0 |
| item 4 | 0 | 0 |
| item 2 | 0 | 0 (after the `import_documents` fix; first run 2, the two `unix.Stream` findings) |
| item 6 | 0 | 0, "0 warnings across 91 source files" |

On the final tree `4f508bd`:

| Gate | Exit | Log |
|---|---|---|
| `make test` | 0 | after item 6 (same tree) |
| `make analyze` | 0 | "0 warnings across 91 source files" |
| `make sanitize` | 0 | ASan+UBSan over every `TESTS` binary and the Python suite, including the new tests |
| `make tsan` | 0 | 19 binaries, now including `test_draft_controller` |
| `make uitest` | **not run** | see below |

**uitest was not run.** No other `build/scribodo` was running (`pgrep` exit 1), but `IOConsoleLocked` was true
and stayed true through about 11 minutes of polling. A locked console fails uitest whatever the code does (project
note "acceptance runner console lock"), and this stream was allowed one run, so it was left for the coordinator to
run once the console is unlocked. It is the strongest check on item 2 (`main()`/`build_layout`), on item 3's
script-mode host check and on item 6, so the branch should not merge until it exits 0.


## Red-team remediation (same branch)

The coordinator's red team on `remed/main` found five items; each is one commit. Gates: `make test`, `make sanitize`
and `make tsan` exit 0 each; `make analyze` was run only when `pgrep -fl "clang --analyze"` showed no other run
(exit 1, nothing found). uitest is the coordinator's, after merge.

| Item | Commit | What |
|---|---|---|
| 3 | `2e0a532` | deterministic in-flight draft tests through a write hook |
| 1 | `d99a745` | no draft write in flight when a load recovers strays |
| 2 | `3940161` | a failed draft write is reported once |
| 4 | `9c6fb48` | the environment-write guard's holes closed |
| 5 | `2c34003` | workflow bodies leave main.c (F3) |

Item 3 went first because items 1 and 2 are tested through its hook.

**Item 3.** `draft_controller_set_write_hook` (tests only) runs on the writing thread before each write. The tests block
it on a mutex/condition gate and wait until the writer is inside, so the write is certainly in flight, then exercise
`discard_while_writing`, `switch_while_writing` and `quit_while_writing`. The old test accepted `|| !d.busy` and so
might never run the `discard_pending` branch. The draft test is in the tsan list; it ran clean five times there before
the commit, and in the final gate.

**Item 1.** Only `editor_load_file` calls `doc_recover_strays`, and only `open_path` calls `editor_load_file`, so
`open_path` is the one load path; it now calls `draft_controller_before_load` before the load. That function **waits
for the write and then discards** (unless the draft holds unsaved text of the document being loaded), rather than
discarding and then waiting as suggested. A discard would also remove the whole draft of a dirty buffer being
reloaded. Under the synchronous writer that draft survived the reload as a recovered copy, and waiting first keeps
that. The race the red team found (Cmd-S, write in flight, the same document opened) ends with no draft at all,
because the buffer is clean. `test_load_while_writing` covers both cases with the write held in flight; a dirty reload
recovers the complete text.

**Item 2.** `adopt` records a failed write with its errno; `draft_controller_take_failure` returns it once per run of
failures (a successful write or a discard re-arms it), and `draft_tick` shows "Couldn't keep a draft of the unsaved
text beside the document (<reason>); save to keep it". The status line is not covered by a UI script (nothing in the
fixtures makes the draft write fail); the controller's once-per-run rule is unit-tested.

**Item 4.** `tests/test_env_writes.py` now scans headers as well as `.c`/`.m`. It flags any mention of
`setenv`/`unsetenv`/`putenv`/`_NSGetEnviron` (a call, a `#define` alias, `&setenv`) and any assignment through
`environ`, outside `startup_env.c`. Self-tests cover each spelling the red team showed passing, and reads that must
not trip it (`posix_spawn(..., environ)` in three files). The ordering check now requires `startup_env_prepare` to be
`main()`'s first call, which no phase split can break. In `startup_env.c` a second prepare and any write after the
seal now `assert`. Every build this tree makes keeps assertions; with `NDEBUG` the old refuse-and-report remains.
`test_startup_env` checks all three abort in a forked child and leave the parent's environment untouched.

**Item 5 (F3).** `src/main.c` goes from 4,912 lines to **1,799**. Moved:

| File | Lines | Contents |
|---|---|---|
| `src/app/app_internal.h` | 506 | `App`, its unions and static asserts, enums, colours, macros, `MainLoop`/`FrameRects`, prototypes of the 77 shared functions |
| `src/ui/layout.c` | 430 | `build_layout`, `layout_*`, `update_labels`, the Clay helpers, `element_rect_str`: all 31 `CLAY(` sites |
| `src/app/script_host_app.c` | 340 | the `host_*` adapters, `scripted_history_read`, `script_host_setup` |
| `src/app/app_lifecycle.c` | 378 | startup phases, fonts and raygui style, `app_destroy` |
| `src/app/chat_workflow.c` | 710 | `send_chat`, `on_message_done`, history windows, attachments, names noticed in chat |
| `src/app/document_workflow.c` | 349 | `finish_prompt`, new/rename/import/reveal |
| `src/app/record_workflow.c` | 578 | notes, canon, entities, observer findings |

The move was done by a script that cuts top-level units (with their doc comments) by name. Every non-blank line of the
old main.c appears in the new files; the differences are `static` dropped from the 77 shared functions, the includes,
the file headers, and two forward declarations no longer needed. Functions that stayed private to a new file stay
`static` there.

Deliberately left in main.c:
- the status line, the project lists refresh, the memory controls, and the store, import and backup polls. The
  services stream edits `store_try_later` and the loop's services-started status; keeping them here keeps that merge
  clean.
- the reviewed-edit glue (propose/accept/reject/restore), `open_path`/`open_project`, the model-settings panel, the
  draft adapter, the welcome screen and the recall panel. These are small adapters the frame loop calls directly.
- the frame loop (`frame_step`, the `frame_*` phases, `main()`), which is what a composition root is.

Longest in main.c now: `frame_left_click` 98, `main` 80, `draw_empty` 74.

Gates on `2c34003` (exit status read from the shell):

| Gate | Exit |
|---|---|
| `make test` | 0 |
| `make analyze` | 0, "0 warnings across 97 source files" (no other analyzer running: `pgrep` exit 1) |
| `make sanitize` | 0 |
| `make tsan` | 0 (includes `test_draft_controller` with the held-write tests) |
| `make uitest` | not run here; the coordinator runs it after merge |

Every item ran `make test` (exit 0) before its commit.

## Second red-team pass (one commit)

The coordinator verified items 1, 3, 4 and 5 closed (uitest 409/409 on `e2615b8`). Still open: item 2, missing tests,
and a way around the environment guard.

- **Failure reporting re-arm.** `failing` was cleared only by a success or by `discard`, and `discard` runs only when
  a draft file exists or a write is in flight. After a failed first write, which leaves no draft, a save or a
  document switch left `failing` set. Fail, save, fail was silent, and so was fail on A then fail on B. Now the
  tick's clean/closed/switched branch and `draft_controller_before_load` clear `failing` unconditionally. Because a
  failed first write leaves the controller owning no document, the switch branch cannot see a change of document;
  `adopt` therefore also starts a new run when the failing document differs (`failing_doc`).
- **Tests added**, each checked to fail when the line it guards is removed:
  - `success_rearms_report`: fail, then succeed with no discard, then fail on the same document. Writes fail through
    a folder made read-only, so it is the same document every time.
  - `failure_runs_without_a_draft`: fail, save, fail; fail on A, then on B; a load re-arms; still once per run.
  - `load_discards_other_documents_draft`: `before_load` with the draft on another document.
  - `tests/test_draft_call_sites.py`, a static check: `doc_recover_strays` is called only in `editor_load_file`;
    `open_path` is the only caller of `editor_load_file`; it calls `draft_controller_before_load` first. Removing the
    call fails it.
- **Object-level environment guard.** `tests/test_env_symbols.py` runs `nm -u` over every product object
  (`CORE_OBJS`, `LLM_OBJS`, `APP_OBJS`) and fails if any object other than `startup_env.o` references `setenv`,
  `unsetenv`, `putenv` or `_NSGetEnviron`. It also checks that it sees `startup_env.o`'s own `_setenv`, so it cannot
  pass by finding nothing. `make test` now depends on those objects and runs it after the unit binaries. A scratch
  object that token-pastes `set##env` fails it. `dlsym("setenv")` still evades both checks; nothing in `src/` calls
  `dlsym`.

Gates on this commit (exit status read): `make test` 0, `make sanitize` 0 (the symbol guard ran over the ASan objects
too), `make tsan` 0, `make analyze` 0, "0 warnings across 97 source files". Analyze was run after `pgrep -fl "clang
--analyze"` returned 1; on the first try another stream's analyze was running, so it waited.
