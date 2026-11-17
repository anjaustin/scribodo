# REDTEAM-024: adversarial review of archive, groups and delete for chats

**Date:** 2026-11-17
**Reviewed tree:** `a56a34d` (archive), `7bc42f0` (groups) and the uncommitted delete.
**Direction:** the author asked for archive, named groups and delete, with delete forgetting what memory learned
from the chat; the change was then red-teamed under the project's usual rule.

## Method

One read-only reviewer was given the code and the requirements, not the author's conclusions, and traced each
finding end to end before reporting it. Each finding was then checked against the code. Each fix has a test that
fails without it.

## Findings and remediations

**R1 — P1: the observer could bring a deleted chat back (fixed).** A delete that landed while the observer was
partway through a job on that chat (about 27 s a call) was followed by the observer's commit. The finding was
inserted with its chat link set to NULL but its quote intact, entity names were created from it, and
`semantic_index_text` re-indexed the chat's text after the pending purge had already run. A status change or
re-filter on a finding deleted meanwhile recreated it through `INSERT IGNORE`. Fixes, in the store, so no caller
can bypass them:
- `sdb_record_append` refuses a finding whose chat source is not a chat of the project.
- It refuses a finding's next version when the finding no longer exists.
- `semantic_index_text` refuses a chat that does not exist, and if a delete commits while the point is being
  written, removes the chat's points again.
Pinned in `tests/test_memory_gc.c` (`chat delete forgets what was derived`) and `tests/test_db.c` (`semantic`,
`backends`). One existing check changed: a finding citing another project's chat used to be kept with no chat; it
is now refused.

**R2 — P2: a chat whose delete was waiting could be reopened, then deleted with its new messages (fixed).** While
the memory job is busy the delete waits; opening that chat is now refused ("That chat is being deleted").

**R3 — P2: copies outside the store keep the text (documented).** The files set aside when a project moved into the
store (`.scribodo/legacy-*`), exports and backups made before the delete keep the chat. Memory reads none of them.
The user guide says so.

**R4 — P2: in the file store, the status line overstated a delete (fixed).** Without the project store only the
chat's file is removed; the message now says that, not "with what memory learned from it".

**R5 — P3: a waiting delete was lost on quit (fixed).** Closing with one pending asks once, as unsaved changes do.

**R6 — P3: failed index purges were seldom retried (fixed).** Besides the memory job's maintenance, the app retries
listed purges every five minutes while a store project is open.

**R7 — P3: an index collection with an unusable name was skipped but counted as purged (fixed).** It now counts as
not purged, so the chat stays listed.

**R8 — P3: a second Scribodo with the deleted chat open can recreate it by sending (open).** Its first append
re-creates the conversation, holding only the new message. Two instances on one project are outside the app's
supported use; recorded here.

**R9 — P3: setting lines could roll back another process's modified time; only 16 groups could be closed
(fixed).** The time is restored only when the file holds exactly the new line more than before. Up to 64 closed
groups are remembered.

## What held up

The reviewer confirmed:
- Every statement in the delete is scoped to the project and the chat.
- The deletion order respects `memory_brief_inputs`' RESTRICT key.
- Notes and canon keep their quotes, with the chat link set to NULL.
- The Qdrant filter keys match the payloads of both index generations.
- Menu positions are right with memory on and off, and actions follow the chat's id, not its row.
- The archive filter covers both of Recall's queries.

## Rounds 2 to 4

The author then asked for the change to be red-teamed until clean. Round 2 had two fresh reviewers, one on the
status bar, archive and groups and one on delete. Rounds 3 and 4 each had one reviewer on the previous round's fixes.
Every finding was checked against the code before it was fixed.

**Round 2, delete.**
- **F1 — P2, a regression from R1 (fixed).** A finding whose chat link was already empty (imported without its
  chat's file, or from an older store) could no longer be dismissed, saved or re-filtered. Only a named chat that is
  missing is refused now.
- **F2 — P2, R2 incomplete (fixed).** A chat being deleted could still be opened from a Recall excerpt or a
  finding's source, and a history load in flight could show a chat that had just been deleted. Every way in now goes
  through one refusal. A load that finishes after the delete is dropped. A delete is refused while that chat's history
  loads, and is cancelled if the chat is open again.
- **F3 — P3 (fixed).** In the file store, the confirmation now says the observer's findings stay.
- **F4 — P3 (fixed).** A rebuild that met a deleted chat counted it as failed and forced another full rebuild. It is
  skipped now.
- **F5 — P3 (fixed).** The observer reported a refused finding as an I/O error and paused for a minute. A refused
  append now sets `records_append_gone()` and the finding is dropped.
- **P3 (fixed).** The purge now also covers the old `memory_v1` collections.
- **P3 (fixed).** A failed re-purge after an index race re-lists the chat.
- **Accepted (P3).** An entity created in the milliseconds between a finding's commit and its mention row can
  outlive a delete. The delete runs on the frame thread and can take a few seconds against a slow Qdrant; purges have
  no payload index.

**Round 2, status bar, archive and groups.**
- **P2 (fixed).** An archived chat still showed "Observer reads this chat", and toggling it said the observer had
  resumed. It now shows unchecked and refuses the toggle.
- **P3 (fixed).** The context figure is reset after a remote reply and after Scribodo restarts the model.
- **P3 (fixed).** "· N set" no longer appears while a change only waits to apply.
- **P3 (fixed).** The figure now sits with the other figures, so a long status cannot push it off the bar.
- **P3 (fixed).** Group names refuse control, direction and invisible-only characters, and "Archived".
- **P3 (fixed).** The open or closed state of groups is per project, and is pruned when full.
- **P3, docs (fixed).** The GAP-004 and LANDSCAPE matrix figures are updated.
- **Accepted (P3).** In the file store a group is renamed chat by chat; a failure part-way leaves some chats under
  the old name, and renaming again finishes it.

**Round 3.**
- **P3 (fixed).** Narrowing the memory wait let a memory step already running on the chat write its index points
  after the purge. The memory job now reports which chat its current step is on (`memory_job_step_on`), and a delete
  waits for exactly that, never for another chat's work.
- **P3 (fixed).** The name filter refused the joiners that Persian, Indic and emoji names need. They are allowed now,
  blanks of any kind are trimmed, and names already kept are shown under a lenient read.

**Round 4.**
- **P2 (fixed).** A consent change (enable, disable, rebuild) still queued for the chat was not waited for. If the
  delete won, the change retried forever and blocked sending. `memory_job_step_on` now also counts queued and
  in-flight consent changes.
- **P3 (fixed).** A store error during the existence checks was taken as "deleted", dropping a finding or skipping a
  passage. The checks now tell an error from absence.
- **P3 (fixed in round 5).** Names drawn as nothing (joiners, fillers alone) are refused. So is "Archived" padded
  with zero-width characters or blanks, compared with all of them left out.
- **Accepted (P3).** The store layer accepts any name already allowed in storage, so the strict check for new names
  is at the prompt, the one place new names come from. Cyrillic look-alikes of "Archived" pass; only the author can
  type them. Closing a 65th open group does nothing.

**Round 5.**
- **P3 (fixed).** A delete held by a consent change that can never be saved (the store keeps failing, or its
  identity changed) waited with no end and no reason. It is now given up after a minute, with the reason.
- **P3 (fixed).** "Archived" padded with blanks inside zero-width characters still passed, and so did U+061C and the
  tag characters. Names are now compared with all blanks and zero-width characters left out; U+061C and the
  language tag U+E0001 are refused (the other tags: round 6).
- **P3 (fixed).** A dead flag left over from round 2 is removed.
- **P3 (not taken).** A store error during the check after an index write could count a deleted chat as present.
  Treating the error as "gone" was tried and reverted: it would list a live chat whose purge takes its memory points.
  The window (a store error at the moment of a delete) is accepted.
- **Accepted (P3).** Group names saved earlier today under the round-1 rule with outer NBSP or now-refused
  characters read back differently. No such name has left this Mac: the change was not yet pushed.

**Round 6.** The revert above was confirmed safe. Refusing the tag characters had blocked the England, Scotland and
Wales flag emoji; they are allowed again and counted as zero-width, like the joiners (fixed). Look-alikes of
"Archived" in other scripts or in fullwidth pass; only the author can type them (accepted).

Each fix above has a test that fails without it, or a mutant run.

## Verification

After round 1, each with exit status 0: `make test`, `analyze` (0 warnings), `sanitize`, `tsan`, `uitest` (496
checks), `dbtest`, `crashtest` (1,711 checks, 151 rows), `uitest-db` (317 checks).

After round 6, on the final tree, each with exit status 0: `make test`, `analyze` (0 warnings), `sanitize`,
`tsan`, `uitest` (496 checks), `dbtest`, `crashtest` (1,711 checks, 151 rows), `uitest-db` (317 checks). Mutant runs
during the rounds: dropping the consent-queue scan from `memory_job_step_on` fails `test_memory_job`; dropping brief
inputs from the delete order, or ignoring links when pruning entities, fails the delete test.
