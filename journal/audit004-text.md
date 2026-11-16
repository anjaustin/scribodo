# AUDIT-004 remediation: data and text stream

**Date:** 2026-11-15. **Base:** `ed95681` (branch `remed/text`). **Scope:** AUDIT-004 F11 (the chat-title archive in
the file store), F10 (the relation check's splitter defect, then one home for the text rules), F12 (records ids).
Evidence: `build/audit004-text/` (gate logs with their exit status, measurement outputs).

## Item 1: F11, the chat-title archive survives the file store

**Defect.** `sdb_chat_rename` keeps the replaced title in `conversation_titles` (migration 018), but the export wrote
only the current title into the file store's meta line and the importer read only the last title line, although the
file store keeps every rename as a `{"type":"title"}` line. A file-to-store-to-file round trip dropped what the
migration was added to keep.

**Change.**

- File store: `chat_store_title_line_files(dir, id, title, ts)` writes one title line stamped `ts`
  (`chat_store_rename_files` now calls it with the clock, unchanged on disk); `chat_store_titles_files` reads every
  title the file has carried, oldest first (the meta line's, then each title line's, the same lines
  `chat_store_list_files_checked` reads the current title from), each with its line's `ts`.
- Export (`export_files_unlocked`): the meta line carries the oldest archived title, and after the messages one title
  line per rename, in archive order, each stamped with the archived row's `replaced_at`; the last is the current
  title. This is exactly the form `chat_store_rename_files` leaves.
- Import (`import_chats` -> `import_chat_titles`): every title before the current one goes into
  `conversation_titles`, `replaced_at` = the `ts` of the line that replaced it (a line without a time, never written
  by the product, takes the chat's last change). Rows already there for that conversation are kept, not doubled.
- **Migration 019** (`ALTER TABLE conversation_titles ADD COLUMN IF NOT EXISTS seq BIGINT NOT NULL AUTO_INCREMENT
  PRIMARY KEY`, one re-runnable statement). The archive's order could not be held by `replaced_at`: the file store
  stamps renames in whole seconds, so two renames in one second tie and a clock set back reorders, and the table had
  no other column to break the tie. `seq` is insertion order (existing rows are numbered in InnoDB's stored order,
  which is insertion order); the export reads `ORDER BY seq`. Without it the round trip was lossless only when no two
  renames shared a second.

**Column checklist.** New SELECT: `sdb_chat_archived_titles` pins `db_rows_has_columns(&r, 2)`. Importer INSERT: new
(`conversation_id, title, replaced_at`; `seq` is assigned). File-store key: the existing title line with `ts`. Schema
pins: `LATEST_SCHEMA` 19, `MIGRATION_STATEMENTS` 85 and the "19" receipt strings in `tests/test_crash_points.c`;
`db_schema_version(db) == 19` (six places) in `tests/test_db_core.c`; GAP-004's header and migration row counts.
**Not edited (outside this stream):** `docs/LANDSCAPE.md` row 8 ("18 embedded migrations") and row 9 ("at schema 18",
133 rows) now read 19 migrations and 135 rows; the coordinator should update them.

**Test.** `test_db.c` `chat_title_archive`: a file chat titled "First", renamed through the product's path
("Second", stamped now), then two title lines stamped 1700000000 ("Third", a clock set back; "Fourth", a tie).
File -> store: three archived rows in order with `replaced_at` equal to the next line's `ts`, the row titled
"Fourth". Store -> file: the export's four titles equal the original's, in order, with identical `ts`; messages and the
listed title intact. Store -> file -> store: a rename in the store ("Fifth", sub-second time), export, import as a
second project: the four archived rows equal title for title and `replaced_at` for `replaced_at` (doubles compared
exactly; cJSON writes 17 significant digits when 15 do not round-trip). With the export and import changes reverted
the test fails five checks (checked by reverting `store_import.c` only).

**Gates (item 1 tree).** `make dbtest` exit 0 (`test_db` 11,317 checks, 0 failures; every binary 0 failures;
`build/audit004-text/item1-dbtest.log`). `make crashtest` exit 0: 135 matrix rows, 131 process deaths, 1,496 checks,
0 failures, 778 s; `migration.statement` 85 points and `migration.receipt` 19 points, every one recovered to schema
19 (`item1-crashtest.log`, `item1-crash-matrix.tsv`). `make test`, `make analyze`, `make sanitize`: below.

## Item 2: F10 defect, the relation check's splitter

**Defect.** `rel_next_sentence` (and the arm's `SENTENCE_END`) treated only Mr., Mrs., Dr. and St. as titles and knew
no initials, so "Ms. Bennet" and "James K. Powell" ended a sentence mid-name; the observer's splitters do not. A cut
statement loses its own negation: "The daughter of Ms. Gardiner is not Lydia." was checked as "The daughter of Ms."
(flagged, unsupported) beside a negated remainder.

**Change.** Both sides take the observer's rule: a period after one of the fifteen titles (Mr Mrs Ms Dr St Capt Col Lt
Gen Prof Sgt Jr Sr Rev Hon, case-sensitive as before) or after a single capital (an initial), each at a word boundary,
ends no sentence. C: `rel_next_sentence`; Python: `TITLES` and one fixed-width lookbehind per title plus
`(?<!\b[A-Z]\.)` in `tools/answer_relation_arm.py`. Tests on both sides: the two cut statements above now read as
negated and unflagged, and a real sentence end after "Capt. Powell ..." still separates the next sentence.

**Measured offline (no model).** Inputs: every captured composer reply under `build/` (46 capture directories,
`build/audit004-text/item2/capture_dirs.txt`, 440 replies) plus the 230 rows of
`tests/fixtures/answer_relation/replies.json`: 670 replies, 262 distinct question-and-reply pairs.

| measure | old | new |
|---|---|---|
| sentences (arm, prose of every reply) | 5,095 | 5,095 |
| replies whose split changed | | 0 |
| replies whose phrases or outcomes changed (arm) | | 0 |
| replies whose verdict changed (arm: flagged, first unsupported phrase, phrase count) | | 0 |
| C `answer_relations` (old object vs new, probe linked against the product objects): rows differing | | 0 of 670 |
| C new vs arm new (checked count and unsupported phrase) | | 0 of 670 differ |
| replies flagged (C new) | | 63 of 670 |

No recorded reply contains a title outside the old four or an initial before a sentence break, so no statement and no
verdict changes on recorded data: no regression, and the fixture needs no regeneration (C and arm still agree on all
230 rows). The defect is real but latent in the recordings; the unit tests pin it.

**What the rule does on manuscript prose** (the arm's splitter over the three novels' chapters, not what the check
reads; `item2/rel_manuscripts.txt`): sentences 2,004 to 2,003 (Mars, "K."), 3,161 to 3,160 (P&P, "P."), 3,744 to
3,737 (Wuthering Heights). Most of Wuthering Heights' joins are the pronoun: `said I. "Proud people ..."` reads as an
initial. This is the observer's rule as it stands (its `obs_sentence_end` makes the same join); a reply rarely ends a
sentence with "I." (none of the 670 do), and a join can only widen a sentence, so the cost in the check is a negation
in the following sentence reaching back across the false join. Recorded here rather than tuned: changing the initial
rule changes the observer's measured splitter too and belongs to a measured change of both.

**Decision.** Ship: 0 of 670 recorded replies change, the C/Python parity holds, and the cut-statement defect is
closed. Reproduce: `python3 -I build/audit004-text/item2/rel_measure.py <worktree> <dir holding old/> <build>
<capture dirs...>`; the C probe with `relprobe_build.sh` (old object compiled from `git show ed95681:...`).

**Gates.** Item 1 commit (`85c2e19`), the other items' changes stashed: `make test` exit 0, `make analyze` exit 0 (0
warnings, 87 files), `make sanitize` exit 0 (`build/audit004-text/item1-{test,analyze,sanitize}.{log,exit}`). Item 2:
below.
