# History-window adversarial review and remediation

**Baseline:** `9e714e0` (DB chat windows and bounded layouts).

**Scope:** window/action identity, ownership and allocation boundaries, invalid
navigation, and unsaved DB chat lifecycle. The unrelated sample-document edit is
preserved and excluded. This review does not certify the unfinished memory MVP.

## Findings

### Stale presentation could select a different reply or action

After a window reset/automatic eviction, input hit-testing used newly positioned
rows before the replacement had been drawn. A click on the previously visible
Note link could therefore act on another reply. Changing document-application
availability also moves Note into/out of Apply's coordinates. Wheel+click used
the scrolled positions instead of the positions the author had seen.

Three headless pointer regressions failed on the baseline. The view now requires
a matching presented chat revision, layout revision, viewport and scroll position
before accepting source/action clicks. Window resets, explicit reveals,
availability/name changes and scrolling invalidate presentation; layout rebuilds
and allocation failures advance the geometry revision. New rectangles are
published by drawing, and a redraw re-enables stable actions. Header clicks are
also clipped to their own viewport.

An action in a transition frame is **ignored, not replayed later**. The author
may need to click again after a fresh stable frame, or stop/wait for generation.
Input composition and the independent Stop control remain available. The
existing UI-thread command-ownership contract is still required; this is not
support for concurrent UI command senders.

### Visible-row eviction retained an archive-sized allocation

Trimming a 10,000-row adopted conversation to 64 rows freed strings but retained
the 10,000-slot message array. Clearing the chat retained that allocation too.
Thus visible count/body-byte checks alone missed retained metadata capacity.
Two new assertions failed before remediation.

Idle trimming now shrinks excess capacity toward 128 slots (or enough slots to
preserve retained unsaved data). A refused shrink leaves data intact and is
retried even when the transcript version has not changed. Single and persistent
allocation-failure tests check preservation and eventual retry. Persistent
allocator refusal can still retain the old allocation: no hard cap is claimed
under that failure condition.

Clear joins the worker and releases message slots and completed request/context
strings, rather than retaining large prior request buffers until the next send.
Diagnostics check capacity and retained request text separately from visible
rows. These are client ownership releases, **not secure erasure**, raw-history
deletion or eviction of the model server's internal caches.

### Empty navigation erased the displayed window

Before-first and after-last requests returned successful empty pages with no
navigation flags. Applying such a result replaced the visible conversation with
a blank window. Two real-MariaDB regressions failed on the baseline.

Empty Older/Newer/Focus requests now fail without publishing a replacement.
Only Latest can successfully represent an empty conversation. This also keeps a
stale boundary request from erasing the current view after external changes;
it does not create a transaction snapshot against arbitrary historical edits.

### Project switching and closing bypassed unsaved protection

Paging guarded unsaved DB turns, but the project-switch loop still cleared them
and close considered only prose/records. A forced manager outage during a fake
reply reproduced loss of the unsaved reply on a project switch and exit on the
first close request. New chat also lacked a discard confirmation for this case.

The UI now retains unsaved DB chat with its original project binding when the
document project changes. Source creation and new sends remain guarded; prose
editing can continue. New chat and close require a second action within five
seconds to explicitly discard unsaved DB chat. The notice asks the author to
save/copy what is needed; it does not imply that document Save persists chat.

The regression harness adds a script-only manager-offline control, not a new
production menu or a real service shutdown. It verifies the unsaved turn before
the transition, preservation afterward, refusal of paging, recovery without
blind append retries, and explicit discard. No other project's data is written
as a fallback. This does not make unsaved RAM durable against process death, nor
does it retrofit persistence-state tracking onto file-backed/unbound chats.

### Stopped replies reopened as complete

The second review found that both persistence backends omitted `stopped`.
Reopening a cancelled reply therefore removed the flag used for incomplete-reply
labels and manual Apply review warnings. New round-trip assertions reproduced
one file-store failure and five DB failures (full load, source metadata and three
focused excerpts) before remediation.

Migration 008 adds a separate non-null `messages.stopped` flag. File JSON,
database append/full load, bounded source/window readers and the direct legacy
import path now preserve it independently of error/truncation. Compatibility
export uses the updated full loader/file writer; import/export regressions cover
the flag. Migration tests verify old rows default false and repeated migration
preserves an explicitly stored true value. Old file records without the field
also remain false. **Previously lost stopped state cannot be recovered.**

### Additional input-domain checks

Window replacement now rejects streaming inputs and excerpt offsets that cannot
be safely advanced by a maximum-sized displayed range. Valid SQL windows already
satisfy these constraints; tests cover malformed direct callers without claiming
that such inputs were reachable through the normal database reader.

## Red/green evidence

Regressions were run before behavioral remediation:

| Suite | Baseline result |
|---|---|
| Presentation/action checks | 3 failures in 318 checks |
| Message allocation retention | 2 failures in 3,635 checks |
| Real DB window boundaries | 2 failures in 11,226 checks, plus 37 passing schema checks |
| Unsaved project-switch/control script | 7 failed expectations, including cascading checks after the lost turn |
| Unsaved close script | 5 failed expectations after premature process exit |

The UI counts are not independent defect counts. The final suites include
additional checks beyond these first reproductions. Updated tests also verify a
fresh redraw accepts the intended action and a live fake worker can stream while
the UI updates/draws and persistence callbacks assign sequences.

## Validation

- `make -j8 all test dbtest`: application, headless suites, retention/layout
  allocation sweeps, and real MariaDB/Qdrant integration.
- `make sanitize` and DB integration rebuilt with ASan/UBSan.
- `make tsan`: now includes the headless chat view, its pointer regressions and
  concurrent worker/presentation case, in addition to the existing suites.
- `make uitest uitest-db`: ordinary/store UI, including the new outage controls.
- `make llmtest uitest-real`: compatibility with the pinned local BF16 model.
- `make backuptest`: disposable database/folder disaster-recovery rehearsal after
  the schema change.
- `python3 tools/test_memory_mutations.py`: the existing selector control and five
  weakened-selector regressions remain checked; these are not mutation proofs of
  every new window/lifecycle fix.
- Clang static analysis of adapter/history/view modules, documentation links and
  `git diff --check`.

Local evidence is retained in `build/window-audit-*.log`; generated logs and test
artifacts are not committed. Hosted CI is verified for the pushed commit rather
than inferred from local runs.

## Remaining scope

Explicit navigation still performs synchronous bounded SQL work. An owned
asynchronous loader, cancellation/timeout responsiveness, workload percentiles,
whole-process/service memory and sustained use remain gates. File-backed and
observer/import/export compatibility readers remain full-history paths.
Ingestion/outbox/Qdrant memory projections and generated briefs are not yet
implemented. See [MEMORY-004](MEMORY-004-history-windows.md). “Clean” in this review
means no further unresolved defect found in the reviewed/tested scope, not a
defect-free application or completed memory MVP.
