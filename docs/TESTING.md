# Testing

| Command | What it runs | Needs a display |
|---|---|---|
| `make test` | Unit and headless suites: core modules, real-filesystem file I/O, and the editor driven through raylib stand-ins (including a 200,000-operation fuzz run) | No |
| `make sanitize` | The same suites under AddressSanitizer and UndefinedBehaviorSanitizer | No |
| `make uitest` | Scripted UI tests: the **real app**, real input handling, real rendering, with a fake model | Yes |
| `make tsan` | ThreadSanitizer over the chat, observer, supervisor and storage code | No |
| `make dbtest` | MariaDB/Qdrant integration in throwaway databases (`tools/services.sh up` first) | No |
| `make llmtest` | The model adapter end to end against the real `llama-server`: verification, stale-socket start-up, auth, streaming, injection neutralization, crash and restart | No |
| `make uitest-real` | `tests/ui/real-*.scribo`: the real model through the real UI | Yes |
| `make dependency-report` | Verifies submodule pins, container digests and OSV advisories; lists available upstream updates without failing on them | No |

## Hang protection

Two layers make sure we never wait on a hung process:

1. **`tools/watchdog.sh SECONDS NAME CMD…`** wraps every test target. If the command outlives its limit, the watchdog samples every thread of it and its children into `build/hangs/NAME-<time>.txt`, prints the main thread's innermost frames, kills it, and exits 124. A real example from the chat work:

   ```
   WATCHDOG: 'app-deadlock' still running after 8s; sampling to build/hangs/app-deadlock-093626.txt
       main  (in scribodo)  main.c:752
       chat_view_update  (in scribodo)  chat_view.c:144
       chat_version  (in scribodo)  chat.c:127
       __psynch_mutexwait  (in libsystem_kernel.dylib)
   ```

2. **The script runner's heartbeat.** In script mode, a watchdog thread checks that the main loop keeps producing frames. After 15 s with none, it reports the script line being executed as a failure and aborts, which leaves a crash report with every thread's stack. That re-planted deadlock failed in 16 s; by hand it had taken 10 minutes to notice.

Limits: unit suites 300 s each, UI scripts 120 s each, `llmtest` 600 s, real-model UI scripts 600 s.

## UI tests (`make uitest`)

Each script in `tests/ui/*.scribo` launches `build/scribodo` in a **hidden window**, so it never steals focus and your real mouse has no effect on it. The script gets a fresh copy of `tests/ui/fixtures/` in `build/ui/<script>/work/`, and its screenshots land in `build/ui/<script>/`. The app exits non-zero if any expectation fails.

How input is driven:
- **Keys, modifiers, mouse movement and buttons, the scroll wheel, window close and resize** are injected through raylib's own event playback (`PlayAutomationEvent`) at the start of each frame. The app's normal input handlers (`handle_keyboard`, `handle_mouse`, splitter dragging, the close guard) run unmodified.
- **Typed characters** go through `input_inject_chars()`. raylib's playback can't create character events, and in the real app those come from the OS text system.
- **The clipboard** is private while a script runs, so tests never read or overwrite yours.
- **Shortcuts** match physical key positions in scripts. That keeps results the same on any keyboard layout; real use matches by layout (see `src/ui/input.h`).

The command reference is at the top of `src/app/script.h`. A minimal example:

```
open "$DIR/chapter.md"
click_index 5 2            # double-click inside "fog"
expect selection 4 7
key C cmd
expect clipboard "fog"
key END cmd
type " The end."
key S cmd
expect dirty no
shot saved                 # build/ui/<script>/saved.png
```

Scripts click on text by buffer index (`click_index`, `drag_index`), not by pixel, so they don't depend on font metrics. Expectations that depend on where a line wraps use ranges (`cursor_range`, `length`).

### Running with the display asleep

Creating a window needs an awake display. If the Mac's display is asleep, the app exits with `could not create a window`. Keep it awake for the run with:

```sh
caffeinate -d -u -i make uitest
```

### What the UI tests can't cover

These sit above raylib and need a real-input pass:
- dead keys and ⌥-symbols (composed by macOS before raylib sees them)
- input-method composition
- layout-aware shortcuts on non-US keyboards
- the real system clipboard
- key auto-repeat
- file drops from Finder
- HiDPI changes when moving between displays

## Checking that the tests catch regressions

A passing suite is only meaningful if it fails when behavior breaks. For the key safety and UI behaviors, we reverted each one in a scratch copy of the tree, rebuilt, and confirmed the suite fails. See [`REDTEAM-001-editor-spike.md`](REDTEAM-001-editor-spike.md) for the editor-level results. For the UI suite:

| Behavior removed | Caught by |
|---|---|
| Unsaved-changes close guard | `close.scribo` |
| Discarding keystrokes typed while unfocused | `mouse.scribo` |
| Per-frame pane width clamp | `panes.scribo` |
| Double-click word selection | `mouse.scribo`, `clipboard.scribo` |
| Drag auto-scroll | `scroll.scribo`, but only with *both* scroll paths removed (see below) |

Two lessons from building the harness:
- **Rebuild cleanly between mutations.** macOS's bundled GNU Make 3.81 compares timestamps at one-second resolution, so a file restored within the same second as its last build can leave a stale object behind. The mutation harness waits more than a second before touching files, and checks that the binary actually changed.
- **Treat crashes as errors, not passes or catches.** One mutation run "missed" everything because the display was asleep and every launch aborted. The harness now voids any run where a script didn't report a summary.

The chat feature was red-teamed separately; see [`REDTEAM-003-chat.md`](REDTEAM-003-chat.md) (19 findings, all fixed or documented, 8/8 fixes mutation-verified).

## Reviewed AI edits

- **Unit tests:** `test_core: diff` (word-level Myers diff, with a 300-case reconstruction property and fallback at the edit limit) and `test_llm` (edit-reply cleaning, the edit request, fake-model edits, revision store integrity). `test_editor: ranges` covers range read, replace and find; `test_editor: anchor` covers the tracked passage through edits, undo and redo.
- **`edits.scribo`** (fake model) covers:
  - select, ⌘K, review, Accept and the revisions it records;
  - restoring from History, then undoing the restore;
  - Reject;
  - a stale passage refused, and a moved passage re-anchored;
  - Esc out of edit mode;
  - a revision recorded on save;
  - read-only documents refusing edits;
  - the red-team 004 regressions: ⌘↩ typing nothing, the wrong-copy and mid-word cases, stopped replies, a failed "before" revision.
- **`real-edit.scribo`:** a real LFM3 rewrite reviewed and accepted through the UI.
- **Mutation check:** four safety guarantees, each broken in turn: applying without Accept, Reject applying anyway, applying over a stale passage, and skipping the "before" revision. All four were caught. The first run "missed" the most important one, because the mutated source didn't compile and the harness re-ran the old binary. Mutation harnesses must check the build and that the binary changed.
- **Red-team:** see [`REDTEAM-004-edits.md`](REDTEAM-004-edits.md) (17 findings, all fixed; 18 of 20 mutations caught, both misses documented).
- **ASan caught an out-of-bounds read** in the diff tokenizer: `memcmp` against the punctuation table near the end of the text. It's fixed by bounding the comparison.

## Notes and canon

- **Unit tests:** `test_llm: records` covers:
  - versions numbered per entry, with every version kept, most recently changed first;
  - document and chat sources round-tripped;
  - snapshots reused when the text is unchanged;
  - a torn line skipped, and the next record starting on its own line;
  - ids and source names that look like paths refused;
  - hand-edited lines with missing numbers.
- **`notes.scribo`** covers:
  - a selection saved as a note with its exact source;
  - editing (⌘S, and saving on Close);
  - opening the source, including after the passage moved;
  - a canon draft that stores nothing until Commit, and needs a second Close to discard;
  - Revise, history, Retract and Recommit;
  - chat replies as notes and canon drafts;
  - deleting, and blank notes that are never stored.
- **Mutation check:** 9 of 9 caught:
  - canon discarded without confirmation;
  - note edits lost on Close;
  - canon stored when a draft opens;
  - deleted notes still listed;
  - a moved source not found;
  - versions always 1;
  - a torn line not terminated;
  - source paths trusted;
  - snapshots not reused.
  A tenth, invalid mutation (it didn't compile) was voided by the harness and rewritten.
- **UBSan caught a real bug:** missing JSON numbers (a document source has no `message` field) read as NaN and were cast to integers. The revision index had the same latent bug. Both now use a checked reader.

## Observer

See [`OBSERVER-001-design.md`](OBSERVER-001-design.md):
- unit tests for evidence matching and an end-to-end fake-backend run;
- `observer.scribo` and `real-observer.scribo`;
- ASan/UBSan and the new `make tsan`, all clean;
- 10 of 10 mutations caught.

Red-teamed in [`REDTEAM-005-observer.md`](REDTEAM-005-observer.md): 16 findings, all fixed, 20 of 20 mutations caught.

UI scripts start with the observer **off**, so other scripts' file expectations stay deterministic. A script turns it on with `observer on` and waits with `wait_observer`.

## Projects and documents

- **`projects.scribo`** and **`projects_rt.scribo`** cover:
  - the welcome screen;
  - opening projects;
  - New document, Import, and Rename via the context menu;
  - modal input;
  - sources across renames.
- **`make cstest`** runs `case_sensitive.scribo` on a temporary case-sensitive APFS image.
- Red-teamed in [`REDTEAM-006-projects.md`](REDTEAM-006-projects.md): 13 findings, all fixed, 13 of 13 mutations caught.

## Chat attachments

- **Coverage:**
  - `attachments.scribo` and `attach_many.scribo`;
  - `test_llm: multi_doc_request`: fair budget sharing, cut marking, neutralization, and protection against forged framing.
- **Red-team:** [`REDTEAM-007-attachments.md`](REDTEAM-007-attachments.md): 12 findings, all fixed; 9 of 9 mutations caught.

## Project store

**`make dbtest`** runs against the local MariaDB in throwaway databases. It covers:
- migrations, including 003 applied to a database that already has data;
- prepared statements, with hostile text and a 3 MB value;
- NULLs, foreign keys, rollback and cascades;
- stable ids and byte-exact names (trailing spaces included);
- reconnecting after the server drops the connection;
- the revision, record and chat backends: History in recorded order whatever the timestamps, and sources never linked across projects;
- the importer: revisions, notes, canon and chats, a rename made before the import, idempotence, legacy files kept, and paused mode;
- import safety: one importer at a time, a stale project row not reused, a copied folder detected and forked into its own project, a folder moved across volumes kept as the project, and a project missing from the store restored from its saved files;
- the entity graph: name normalisation, mentions and links from findings, an unnamed end left unlinked, dismissal, idempotent backfill, and no links across projects;
- semantic retrieval against Qdrant with the fake embedder.

It's also clean under ASan/UBSan.

**`make uitest-db`** runs the app six times on one database (`tools/services.sh up` first):
1. A project with existing files is moved into the store, then notes, canon, a chat, a rename and the observer are used.
2. After a restart everything comes back, the chat resumes (SOW acceptance 1), and the observer reads nothing twice.
3. The project is backed up and restored into a separate folder; records, chats, attachments and History survive.
4. The entity graph (`5_entities.scribo`): the fake observer's relationship finding ("Zoë is Mara's sister.") becomes two entities and a link. "›" goes to the other entity, and the relationship opens its passage.
5. A copy of the folder (`cp -R`) is recognised as a copy and paused (chat refuses). "Make it its own project" brings over the original's documents, chats and canon under a new project.
6. With the database unreachable, documents stay editable and saveable. Chat, rename and New pause, and nothing falls back to files.

## Findings from the chat work

Each of these was caught by the suites and has a regression check:

- **Self-deadlock in the chat view.** It took the chat lock and then called a function that takes the same lock, freezing the first frame the chat pane drew. Fixed; this is the bug the hang protection exists for.
- **Typing into the chat input never worked.** The unfocused document editor drained the shared typed-character queue before the chat input read it. The drain now happens once per frame at the app level, after every text view has updated.
- **ID collision.** The Explorer's first chat row (`CLAY_IDI("Chat", 0)`) hashed identically to the chat pane (`CLAY_ID("Chat")`), so clicking anywhere in the chat pane opened a saved chat. Rows are now `ChatItem`/`DocItem`, and `chat.scribo` clicks the pane and checks that nothing opens.
- **Stale element boxes.** `Clay_GetElementData()` returns the last known box of an element that's no longer declared (a closed pane), which made a closed Observer look open. `clay_element_current()` checks Clay's per-layout generation before any box is trusted.
- **Pane clamping at minimum width** could squeeze the document to ~155 px. It now shrinks chat, then the Observer, then the Explorer, and finally closes the Observer.
- **raygui renders with the primary font face only,** with no fallback. Button labels must avoid glyphs Verdana lacks; the Observer button showed "?".

## Findings from the UI suite

- **The unsaved-changes dot rendered as a missing-glyph box.** "●" (U+25CF) and "⌘" (U+2318) were outside the character ranges we load. Fixed by loading the Geometric Shapes and Miscellaneous Technical blocks. Found by screenshot, not by an expectation.
- **The app aborted inside GLFW when no display was available.** It now checks `IsWindowReady()` and exits with a clear message.
- **Drag auto-scroll has two mechanisms:** explicit edge scrolling, and keep-the-caret-visible scrolling that fires on every drag update. Either one alone auto-scrolls; together they scroll faster. This isn't a bug, but worth knowing before tuning scroll speed.
- **Harness bug:** screenshots taken just before `EndDrawing` missed the last draw batch (the Save button). The capture now flushes raylib's batch first.

## Offline, backup and remote

- **`make offlinetest`** (acceptance 8): everything inside a macOS sandbox with no network but this machine (`tools/offline/no-network.sb`), with controls proving the sandbox blocks outside hosts. It covers the store suite and the real models (chat, edit, the observer on files and on the store, `llmtest`). It needs the real model to itself.
- **`make backuptest`:** a disaster rehearsal, plus crafted backups that must be refused:
  - planted files;
  - links;
  - a shell escape;
  - another database;
  - a file that isn't a dump;
  - a missing `--to`.

  A `--replace` must also drop the stale index.
- **`make remotetest`:** the remote model against a stand-in provider. It checks what actually reached the provider: one request, with its key and document, nothing from before the switch, and nothing after switching back.
