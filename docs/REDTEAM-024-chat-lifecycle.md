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

## Verification

After the fixes, each with exit status 0: `make test`, `analyze` (0 warnings), `sanitize`, `tsan`, `uitest` (496
checks), `dbtest`, `crashtest` (1,711 checks, 151 rows), `uitest-db` (317 checks).
