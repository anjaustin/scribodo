# REDTEAM-022: adversarial review of the whole product, end to end

**Date:** 2026-11-14
**Reviewed tree:** `69e191e` (the three steps of the day: the observer's wait
mode, the conflict gate measured and gated off, the author-declared note turn)
with the default-path change in the working tree (local memory and the
verified pipeline on by default, `journal/answer-cycle4_default_path.md`).
**Direction:** the author: "make sure all the best code and features are on the
default path. Then, red-team the project end-to-end, remediate your findings,
verify remediations and iterate until pristine."

## Method

Six read-only reviews ran in parallel over the whole tree, each with the
project's three questions (what does content make this code do that it should
not; what happens on the worker thread when a deadline, a cancel or an
allocation fails; which claim could be wrong without a test noticing): the chat
and answer path (`src/llm`), the observer and its replay harness, the stores
and jobs (`src/app`), the workbench (`src/main.c`, `src/ui`, `src/core`), the
verification system and the record (Makefile, workflows, tools, tests, the
documents' numbers), and the product promise followed across modules
(authority, forgetting, disclosure, privacy, durability; MEMORY-015 §4). Each
reviewer confirmed a finding by tracing the full path before reporting it and
marked what it could not rule out as plausible. Every confirmed finding was
re-read here against the code before it was fixed; the fixes were then run
through every gate (below) and the fake-path yardstick.

Forty findings were reported, five of them the same defect seen from different
sides (the note mark lost on reopen). Four were P1 or P2 against the promise,
nine P2 against behaviour, the rest P3. All were remediated on this tree except
the three recorded under "Deferred", each with its reason.

## Findings and remediations

### The promise (MEMORY-015 §4)

**R1 — the author's note mark was lost when a stored chat was reopened (P2, fixed).**
`store_chat_history.c`: the windowed loader, the one every store project uses
to open and page a chat, never selected `messages.is_note`; only the full
loader (file projects, export) did. A reopened chat showed every note as a
question, and the gate would have armed on a regenerated note turn. The two
`body_sql` statements select the column, the pin is 15 columns, the row reads
it; `test_db` `chat_windows` appends a note and reads it back through the
window. (Reported by three reviewers.)

**R2 — a dead battery lost everything since the last ⌘S (P2, fixed).**
Nothing wrote the editor's unsaved text until the author saved. `draft_tick`
(`main.c`) now writes the whole buffer beside the document every 10 s while it
is dirty and changed, under the save temp's own name
(`<document>.scribodo-XXXXXX`, `doc_write_draft` in `doc_io.c`, fsync'd), one
draft at a time, removed when the buffer is clean. A process that dies before
the author saves leaves the text where `doc_recover_strays` already offers it
on the next load, renamed beside the document, never overwriting anything.
`test_doc_io` `write_draft` pins the writer and the recovery. The Explorer
lists only `.md`, `.txt` and `.markdown`, so a draft is not a document.

**R3 — a chat append that failed was never retried (P3, fixed).** The status
told the author to "keep" the messages, sending was then refused, and nothing
re-appended them. The refused append marks the message (`save_failed`,
cleared by `chat_mark_saved`); `resave_unsaved_chat` runs when the store
answers again and appends exactly the marked messages, in order. (The first
version inferred "unsaved" from a missing sequence; the second pass showed that
re-appended a reply whose done callback had not run yet and a chat loaded from
files before its project moved into the store.)

**R4 — conflict findings carried no disclosure of their measured precision (P2, fixed).**
The badge read CONFLICT in red. It reads POSSIBLE CONFLICT, and a list that
holds one is headed by "Possible conflicts are suggestions: about 1 in 13 was
real on the test novels. Each links to its source." (7 true in 92 shown,
`journal/observer-cycle2_quality.md`). The constant is to be kept with the
journal when the number moves.

**R5 — a note or canon entry made from a finding did not record which finding (P3, fixed).**
`finding_action` FA_NOTE and `store_canon` pass the finding's id in
`FindingInfo.related`, which the store writes to `record_versions.related_id`
(and the file store to the version's `finding.related`); the model origin of an
author-role record is now structural, not only textual.

**R6 — a chat's rename overwrote its title with no history (P3, fixed).**
Migration 018 adds `conversation_titles`; `sdb_chat_rename` writes the previous
title there in the same transaction as the rename (delete nothing, only
archive). `test_db` reads the kept title back. Schema pins 17 → 18; the crash
matrix gains one statement and one receipt (84 and 18).

**R7 — the remote indicator did not say the profile name goes too (P3, fixed).**
The toolbar line names chat, attached documents, edit passages and the
author's profile name.

**R8 — a scripted run read the author's real `remote.json` (P3, fixed).**
Scripts keep their own in the fixture folder, like the profile; a provider's
name and host can no longer reach a failed run's uploaded artefacts.

**R9 — MariaDB's durability runs through Docker Desktop's virtual disk (P2, recorded).**
An `fsync` inside the Linux guest reaches the Mac's disk only if the hypervisor
flushes; GAP-004 names the limit and the README's saving section now says it
beside the backup instructions. The mitigation the product has is the backup;
a periodic export to the project folder is the next step if the author wants
one (deferred, see below).

### The chat and answer path

**R10 — pipeline on, memory consent off: the request was rebuilt around "No verified passage was found" (P2, fixed).**
`memory_pipeline` passed its guard when the scope read disabled, ran no search,
and still rebuilt the request with the composer told the manuscript gave
nothing, armed the gate on zero spans and wrote a receipt with an empty status.
With the pipeline now on by default this was the shape of every conversation
the author had switched memory off in. The stage returns before touching the
request when consent is off or no search could run; the recall stage then
reports "Local memory disabled" as before.

**R11 — every score failing was presented as a search result (P2, fixed).**
When the reranker refused or was unavailable for all pairs, the preface still
said no passage was found. The stage keeps the plain request and the notice
says "none accepted by the reranker; the plain request was sent".

**R12 — the reranker clipped the document but not the question (P2, fixed).**
A pasted paragraph ending in a question overflowed the 2,048-token slot, every
score failed, and R11 followed. `rerank_prompt` clips the question to 1,000
bytes on a character boundary.

**R13 — the edit turn's stored label was cut at 599 bytes, possibly inside a character (P2, fixed).**
The cut row then failed MariaDB's strict `utf8mb4` check, the chat read as
unsaved and sending was refused. The label is allocated to the instruction's
length; the author's record keeps the whole request.

**R14 — `chat_destroy` freed the gate strings before joining the worker (P3, fixed);
R15 — the fake worker never cleared an armed gate (P3, fixed); R16 — the
sampling settings were read outside the lock (P3, fixed); R17 — a failed span
copy leaked its name (P3, fixed); R18 — the receipt's refusal marker was taken
at its first occurrence, which manuscript text could supply (P3, fixed: the
last); R19 — the pipeline rebuild counted tokens through a budget already spent
(P3, fixed: the byte estimate, stated).**

### The observer

**R20 — chat activity during discovery or a rebuild was charged to the next job as a cancellation (P2, fixed).**
`cancelled_by_chat` leaked out of the three worker paths that `continue`;
the next job, even a completed one, doubled the wait and counted a cancel that
never happened, which would also have corrupted the owed soak measurement. The
flag is cleared where a job starts.

**R21 — the judge statement's quote was clipped by bytes, cutting inside a character (P2, fixed).**
`%.300s` on a quote the schema bounds at 300 characters produced invalid UTF-8;
the server refused the request and the statement was never checked. The
recorded Mars runs hold a 305-byte quote ending in an em dash. `obs_statement`
clips by characters; `test_observer_text` pins a dash straddling byte 300.

**R22 — in-job conflict dedupe keyed chat conflicts on the offset alone (P3, fixed: the quote too);
R23 — the wait-mode backoff survived a project switch (P3, fixed: reset);
R24 — the wait-mode test slept a fixed 300 ms before acting (P3, fixed: it
waits for the job to be running); R25 — OBSERVER-001 still said 1.5 s (fixed).**

### The stores and jobs

**R26 — the Explorer dropped documents past the 200th silently (P2, deferred, see below).**
**R27 — the migration receipt was a plain INSERT, so two processes migrating at once failed one (P3, fixed: INSERT IGNORE).**
**R28 — a chat path over 1,012 bytes was cut into a sibling directory (P3, fixed: PATH_MAX + 32, refuse a cut).**
**R29 — a failed attachment link was swallowed and the turn stored without it (P3, fixed: the append fails, as the importer's does).**
**R30 — a damaged newest snapshot blocks recording an identical save (P3, considered, kept).** The reviewer proposed writing a fresh snapshot; the design fails closed so a damaged History entry is reported, not papered over, and `test_llm` `snapshot_reuse_integrity` pins that. The first attempt here changed it and the test caught it; reverted.
**R31 — a recognised record line with invalid fields was dropped silently (P3, fixed: fail closed, as chats do since REDTEAM-010).** Writes to a file project's notes or canon re-read the log first, so one such line now refuses new entries with "Could not save" until the file is repaired; the author is told rather than read short.
**R32 — `db.c` returned allocation failures with the previous error text (P3, fixed: set_err); its migration comment said "one transaction" (fixed).**

### The workbench

**R33 — the Note toggle drew active in edit mode though an instruction is never a note (P3, fixed: hidden, and the composer takes the width).**
**R34 — the toggle was mouse-only (P3, fixed: ⌘⇧N in the composer).**
**R35 — the script runner's `wait_chat` timed out silently (P3, fixed: it reports), and `memory_sources_not` / `recall_not` passed when no value existed (P3, fixed: a negative expectation needs a value).**
**R36 — a wrap that ran out of memory left a partial layout in use (P3, fixed: the layout stays dirty).**
**R37 — the composer's hint was clipped by the toggle (fixed: shorter).**

### The verification system and the record

**R38 — the crash matrix had no floor (P1, fixed).** A transition with zero
kill points passed ("0 points, every one recovered"); a refactor that dropped a
`crash_point` or the compile flag would have kept GAP-004 green. Every
exhaustion branch now requires at least one death, the migration rows require
exactly one per statement (`MIGRATION_STATEMENTS`, 84) and per receipt
(`LATEST_SCHEMA`), the disk row's skip fails unless
`SCRIBODO_CRASH_SKIP_DISK=1`, the matrix label reads the schema constant (it
said "16 receipts"), and `make uitest` fails with no scripts.

**R39 — a document name could carry the receipt's field separator (P2, fixed).**
The writer maps `|` to `/` in the document field; `answer_span_tolerance.py`
accepts names with spaces (its `\S+` dropped every receipt of a manuscript
named "Chapter One.md"); `test_answer_pipeline` pins a piped name.

**R40 — `observer_lines.read` let a fragment swallow the record after it (P3, fixed: the last physical line is re-read alone).** No recorded number was
affected: every findings file under `build/observer` reads `malformed=0`.

**R41 — stale record cells (P2, fixed):** LANDSCAPE row 17 still said the
budgets were proposed and the VM cap unpinned; rows 8 and 9 contradicted each
other on GAP-004 and carried old counts; the header was three days old;
GAP-004 said "82 statements"; the CI coverage list omitted `crashtest`; the
README's requirements omitted `openssl@3`, `cmake`, python and Docker.

**R42 — the shared runner's second service stack could exceed the pinned VM (P3, fixed):** the compose limits are variables and the acceptance job sets
512 MiB and 768 MiB.

### Found by the verification itself

**R43 — with the pipeline on by default, the fake backend's memory path produced no sources (P2, fixed).**
The first gate run after the default-path change failed seventeen store-UI
assertions: under the fake model the pipeline's fixture scores, nothing can
extract a span, and every memory-on turn was rebuilt around "No verified
passage". The pipeline now runs with the fake backend only when a test asks
for it (`SCRIBODO_ANSWER_PIPELINE=verified`, `answer_pipeline_requested`);
the UI scripts keep testing the recall path, and a real model takes the
pipeline. Two more defects of this session's own making were caught the same
way: the draft fields had been added inside the union that mirrors the
reviewed-edit controller's layout (every edit request went out as a plain
message and the exit freed a garbage pointer), and the window loader's note
assertion ran after the test had stopped the store (a use after free the
sanitizer build located in seconds).

## Second pass over the remediations

Two reviewers read the remediation commit through git objects while it was
being gated. Confirmed and fixed on the tree that followed: `wait_chat`'s new
timeout report named the next command's line (the wait had already advanced
`pc`; it steps back to report); `observer_lines.read` still let a fragment
without a pipe become a record's `type`, and lost a two-line record behind a
piped fragment (the pending physical lines are now kept apart, a record may
begin at any of them, a type with a newline is never complete; five shapes
pinned in `test_observer_faults.py`); `resave_unsaved_chat` could re-append a
reply finished in the same frame the store came online, and a file-loaded chat
after its project moved into the store (R3 above). Plausible points adopted:
a gate armed by a turn whose build failed is cleared at the next build's start;
`sdb_chat_rename` refuses to nest into a caller's transaction, as the append
does; a failed wrap keeps the previous layout in place instead of a partial
one; the Note toggle's disabled state is set before the frame's hit test; the
disclosure line wraps to the pane. Noted, not changed: the draft writer's 10 s
interval is the only knob on its cost for very large documents; the compose
caps are caps, not reservations.

## Deferred, with reasons

- **R26, the Explorer's 200-document cap.** The fix is a `truncated` flag in
  the snapshot and a status line; it touches the snapshot struct that three
  jobs share and the layout in `main.c`, and no author on this Mac has 200
  files in a folder yet. Recorded in PRODUCT-GAPS under GAP-005.
- **R9's mitigation**, a periodic project export to the folder: the author's
  choice of cadence and place; the limit is now stated where the author reads
  about saving.
- **The observer's commit stage** aborts on a send in its one-second window
  (reviewer's P3): finishing the commit under chat activity needs a predicate
  without `cancel` and a measurement of how often it happens; left as the
  next observer item.

## Verification

On the remediated tree (`build/observer/cycle3d-gates/`, every exit status 0
unless stated): `make test` 95,766 C checks and 192 Python tests, 0 failures;
`make dbtest` 18,006 checks, 0 failures; `make crashtest` 133 matrix rows,
129 process deaths, 1,478 checks, 0 failures, with 84 statement and 18 receipt
deaths at schema 18; `make uitest` 409 assertions in 23 scripts, 0 failures;
`make uitest-db` 281 assertions, 0 failures; `make analyze` 0 warnings across 87 source
files; `make sanitize` 95,766, 0; `make tsan` 48,339, 0; the fake-path yardstick
`observer_replay_run.sh redteam022` byte-identical with `realflag` on pp, mars
and heights (717 / 348 / 948), so the observer's statement change alters no
fake-path finding. The first run of the chain failed in three places of this
session's own making (R43) and in two store-UI steps whose negative expectation
had passed vacuously before R35 (both steps follow "memory disabled for this
conversation"; they now state `expect memory_sources_none`); the chain was rerun
after each fix.

Hosted: CI and Trusted acceptance verdicts are recorded in the journal when
the commits are pushed.
