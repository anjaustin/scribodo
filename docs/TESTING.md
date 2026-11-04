# Testing

| Command | What it runs | Needs a display |
|---|---|---|
| `make test` | Unit and headless suites: core modules, real-filesystem file I/O, and the editor driven through raylib stand-ins (including a 200,000-operation fuzz run) | No |
| `make sanitize` | The same suites under AddressSanitizer and UndefinedBehaviorSanitizer | No |
| `make uitest` | Scripted UI tests: the **real app**, real input handling, real rendering | Yes |

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

## Findings from the UI suite

- **The unsaved-changes dot rendered as a missing-glyph box.** "●" (U+25CF) and "⌘" (U+2318) were outside the character ranges we load. Fixed by loading the Geometric Shapes and Miscellaneous Technical blocks. Found by screenshot, not by an expectation.
- **The app aborted inside GLFW when no display was available.** It now checks `IsWindowReady()` and exits with a clear message.
- **Drag auto-scroll has two mechanisms:** explicit edge scrolling, and keep-the-caret-visible scrolling that fires on every drag update. Either one alone auto-scrolls; together they scroll faster. This isn't a bug, but worth knowing before tuning scroll speed.
- **Harness bug:** screenshots taken just before `EndDrawing` missed the last draw batch (the Save button). The capture now flushes raylib's batch first.
