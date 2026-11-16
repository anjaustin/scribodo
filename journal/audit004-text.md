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
Item 2 commit (`275688e`), items 3 and 4 stashed: `make test` exit 0, `make analyze` exit 0, `make sanitize` exit 0
(`build/audit004-text/item2-*.{log,exit}`).

## Item 3: F10 consolidation, one home for the text rules

**Module.** `src/core/text_rules.{h,c}` (in `CORE_SRCS`, and in the context planner's headless test sources):
`text_word_char` / `text_word_boundary` (ASCII \w and \b), `text_is_initial`, `text_title_before(s, dot, titles, n)`,
`text_abbreviation_before(s, dot)` (the sentence rule: a `TEXT_TITLES` title or an initial at a word boundary),
`text_word_in_folded`, and named lists: `TEXT_TITLES` (the fifteen of the observer and, since item 2, the relation
check), `TEXT_TITLES_QUESTION` (Mr Mrs Ms Dr St: the question splitter as measured), `TEXT_TITLES_EXCERPT` (the
context planner's eight, folded), `TEXT_NAME_HONORIFICS` (the relation NAME prefix, pinned to the arm),
`TEXT_RELATION_WORDS` (the relation check's 27, pinned to the arm's KIN), `TEXT_NUMBER_WORDS` (zero to twenty),
`TEXT_TENS_WORDS` (twenty to ninety), and, for the observer stream to adopt, `TEXT_OBSERVER_KIN_WORDS` and
`TEXT_OBSERVER_NUMBER_WORDS` (copies of `observer_text.c`'s frozen lists, NULL-terminated as there; until adopted
they are copies, and `observer_text.c`'s `abbreviation()` / `obs_first_sentence` titles equal `TEXT_TITLES`, the
first case-folded). Different lists were not merged: each module keeps exactly the list it was measured with.

**Migrated.** `answer_pipeline.c`: the three title lists (relation splitter -> `text_abbreviation_before`; question
splitter `honorific_period` -> `text_title_before(..., TEXT_TITLES_QUESTION)`; relation names ->
`TEXT_NAME_HONORIFICS`), the two identical word-character functions (`rel_word_char`, `word_char`) and `rel_boundary`
-> `text_word_char` / `text_word_boundary`, the kin list -> `TEXT_RELATION_WORDS`, the six number-word arrays in
`number_word`, `digit_word`, `compound_number`, `tens_word`, `units_word` -> `TEXT_NUMBER_WORDS` / `TEXT_TENS_WORDS`
(slices where a function used a sub-range). `context_planner.c`: the excerpt splitter's eight-way `strcmp` ->
`text_word_in_folded(..., TEXT_TITLES_EXCERPT)`. Not touched: `observer*.c` (another stream), `context_planner.c`'s
stop-word list (a stop list, not a title rule), `k_rel_not_names`, `k_rel_negation`.

**Byte-identical, proved.**

- End to end: a probe linked against the product objects, old (`answer_pipeline.c` at `275688e`) and new, over 5,220
  inputs (the 670 recorded replies with their spans, turns and questions, and 4,550 manuscript paragraphs of the
  three novels, each with the previous paragraph as span and a random paragraph as question), printing
  `answer_relations`, `answer_question_parts` of the question and of the reply text, `answer_gate` (the number
  words) and `answer_quotations`: the two outputs are identical (`cmp`, 6,016,128 bytes;
  `build/audit004-text/item3/`).
- Function level (`item3/rules_fuzz.c`): the old inline rules against the shared functions at every period of the
  manuscripts (chapters and the full source texts, 18,838 periods), at 675,957 periods of 2,000,000 random strings
  over title letters, initials, digits, underscore, a UTF-8 letter and newline, and every byte value for the
  word-character rule: 0 differences for the relation rule, the question rule, the excerpt rule and the
  word-character rule; the number, tens, kin and honorific lists equal the old arrays element for element.
- The existing tests: `test_answer_pipeline` (299 checks, including the C/arm parity over the 230-row fixture),
  `test_context_planner`, and a new `test_core` `text_rules` case.

No `setlocale` call exists in the tree, so the old `isalnum` was the C locale's ASCII rule, which `text_word_char`
now states without depending on the locale.
Item 3 commit (`3a4368e`), item 4 stashed: `make test` exit 0, `make analyze` exit 0 (0 warnings, 88 files: the new
module included), `make sanitize` exit 0 (`build/audit004-text/item3-*.{log,exit}`).

## Item 4: F12, records ids

**Defect.** `records_new_id_unused` returned void and, once all 65,536 attempts landed on taken ids, left the last
(taken) id in `out`: the caller would write a new record under an existing id and hide the earlier record as one of
its versions.

**Change.** It returns `bool`; on exhaustion it returns false and empties `out`. An empty id is refused by every
record write (`valid_id` in `append_impl`, both backends), so a caller that ignores the result still fails its write
instead of overwriting: the two callers are `observer.c` `job_create` (not this stream's file), where the failed
`records_create_ex` already sets `io_failed`. The observer stream should still test the result explicitly
(`if (!records_new_id_unused(...)) { r->io_failed = true; continue; }` or `break`) when it next edits `job_create`.
Test hook `records_test_id_hooks(fixed_time, max_attempts)` (atomics; 0, 0 restores the clock and 65,536): a fixed
second makes the per-second count deterministic and a budget of 3 makes exhaustion reachable without 65,536 x n
comparisons. `test_llm`: with the next three suffixes taken, the draw fails with an empty id, `records_create_ex`
refuses that id, and the next draw (budget 3 again) lands just past the taken ones. The existing two calls now CHECK
the true result. A comment in `records_new_id` records that its function-scope statics (mutex, last second, next
suffix, started) are an intentional process-global: the per-second count must be shared by every caller in the
process; the coordinator lists it in the register.

**main.c (reported, not edited: another stream is refactoring it).** The five new-id sites (`save_note` ~2219,
"Save as note" ~2319, the reply-as-note path ~2369, `save_canon` ~2480, the finding's `FA_NOTE` ~2611) draw with
`records_new_id` and write with `records_append`, which is not fresh: if the id already exists in the project's log
(another process's id, or a run before a restart, in the same second with the same random start and count) the new
note or canon entry is appended as the next version of the existing record and hides it. The per-process count makes
this a cross-process, same-second collision, probability about n/65,536 per draw: theoretical, as in the observer
before it adopted `_unused`. Recommended change: one helper beside the five sites,

    static bool new_record_id(App *app, RecordKind kind, char *id, size_t idlen)
    {
        Record *taken = NULL;
        size_t ntaken = 0;
        bool ok = records_list_checked(app->store, app->project_dir, kind, &taken, &ntaken) &&
                  records_new_id_unused(kind, taken, ntaken, id, idlen);
        records_free(taken, ntaken);
        return ok;
    }

and at each site `records_new_id(K, id, sizeof(id));` becomes `if (!new_record_id(app, K, id, sizeof(id))) { <the
site's existing failure path: set_status(app, "Couldn't save ..."), free(text), return false / break> }`. The list is
read without the store's lock, so a concurrent writer can still land between the read and the write; closing that
needs the draw and the write under one lock (the observer's pattern), which is the main.c refactor's to decide.

## Final gates (the item 4 commit: all four items, nothing uncommitted)

`make test` exit 0; `make analyze` exit 0 (0 warnings, 88 files); `make sanitize` exit 0; `make dbtest` exit 0 (every
binary 0 failures, `chat_title_archive` ok); `make crashtest` exit 0 (135 matrix rows, 1,496 checks, 0 failures, 530 s;
`migration.statement` 85 and `migration.receipt` 19 points, each recovered to schema 19). Logs and exit files:
`build/audit004-text/final-*.{log,exit}`. Every `make analyze` here ran after 16:59, outside the window in which
another stream's `pkill -f "clang --analyze"` could have killed analyzer processes, and each reported its full file
count. Not run (out of this stream's scope): uitest, uitest-db, backuptest, llmtest, uitest-real, offlinetest, any
real-model run.

## Open, for the coordinator

- `docs/LANDSCAPE.md` rows 8 and 9 still say 18 migrations / schema 18 / 133 rows; they are now 19 / 19 / 135.
- The register should list `records_new_id`'s statics as an intentional process-global.
- `observer.c` `job_create` should test `records_new_id_unused`'s result (today an exhausted draw fails at the write).
- The initial rule joins sentences after the pronoun "I." (Wuthering Heights, item 2); it is the observer's rule too,
  so any change is a measured change of both splitters.
- main.c's five new-id sites: the recommended `new_record_id` helper (item 4).

## Red team of this branch (2026-11-15), remediated

Evidence: `build/audit004-text/redteam/`.

### R1 (medium, safety): a label's or the pronoun's capital was read as an initial

**Defect.** Item 2 made any single capital before a period an initial. In the relation check that joined the next
sentence, and the next sentence's negation then hid the first sentence's wrong relation: "Her father is Tardos Mors
in Chapter V. Lydia is not his daughter." was flagged at `ed95681` and unflagged at `013f879` (C probe, both
versions linked beside the product objects: `redteam/redteam_cases_out.txt`; the same for "as stated in Part I.
That is not in dispute.", "according to Source B. That is not contradicted." and "and so said I.").

**Change.** `text_name_initial_before(s, n, dot)` in `src/core/text_rules.c`, used by `text_abbreviation_before`
(which now takes the length), mirrored character for character in the arm (`abbreviation_at`, and `sentences` now
filters the break candidates of the old regular expression with it, still breaking at any newline run inside a refused
break's whitespace as the C loop does). A single capital is an initial only when: it is not "I"; it is at a word
boundary after spaces or tabs; the word before it is capitalised (leading quotes and brackets ignored, one trailing
period allowed so "J. K." chains) and is not one of `TEXT_LABEL_WORDS` (Chapter, Ch, Part, Book, Volume, Vol, Act,
Scene, Section, Sec, Source, Appendix, Figure, Fig, Table, Plate, Page, Item, Note, Step, Letter, Line, Option, Plan,
Type, Class, Group, Exhibit, Article, Clause); and after the period and whitespace comes a capitalised word that is
not one of `TEXT_SENTENCE_STARTERS` (The, This, That, It, He, She, I, ... 47 words). Titles are unchanged. A refused
initial is a sentence end, the pre-branch behaviour, so every miss of this rule errs toward the old split. Known
misses, accepted: an initial with no capitalised word before it ("J. K. Rowling" splits after "J."), and a label word
outside the list followed by a name ("Vitamin C. Lydia ..." joins).

**Tests, both languages.** The four red-team replies plus Roman numerals ("Book II.", "Act IV."), a letter label
("Appendix C.", "Plan B.") and "said I." each flag "Her father is Tardos Mors" (`test_answer_pipeline`,
`test_answer_relation_arm`); the arm's splitter keeps "James K. Powell" and "Dr. K. Smith" whole and splits
"John K. The rest"; `test_core` `text_rules` checks the rule at 21 periods (names, labels, starters, quotes, the
text's end).

**Re-measured (no model).** On the 670 recorded replies (262 distinct): sentences 5,095 under the base rule
(`ed95681`), item 2's rule (`013f879`) and this one; 0 splits, 0 phrases, 0 verdicts changed against either
(`redteam/rm_old.txt`, `rm_old013.txt`). C: `answer_relations`, `answer_question_parts`, `answer_gate` and
`answer_quotations` over the 670 replies identical against both old versions, and over all 5,220 inputs (replies
plus 4,550 manuscript paragraphs) identical against `013f879`; C and arm agree on all 670 (63 flagged). Novels (the
arm's splitter over the chapters, `redteam/manuscripts.txt`): against the base rule, Mars 2,004 to 2,003 (one name
initial joined), Pride and Prejudice 3,161 and Wuthering Heights 3,744 unchanged; against item 2's rule, the eight
false joins are undone (P&P "P." label 3,160 to 3,161; Wuthering Heights' "said I." and initials before a sentence
starter 3,737 to 3,744). The item 2 paragraph above on "I." is superseded.

**The observer shares the flaw** (not edited; for routing). `obs_sentence_end` and `obs_first_sentence`
(`src/llm/observer_text.c`) treat any single capital as an initial: on "Her father is Tardos Mors in Chapter V. Lydia
is not his daughter." both return the whole text as the first sentence, and the same for "It is as stated in Part I.
That is not in dispute." and `"Proud," said I. "Proud people breed sad sorrows."` (`redteam/observer-probe.txt`,
linked against `observer_text.o`). Adopting `text_abbreviation_before` there is a change to the observer's measured
splitter and should be measured on its replay yardstick.

### R2 (medium): an out-of-range title time broke every later export

**Defect.** `chat_store_titles_files` took any finite `ts` on a title line. A line stamped 1e300 or -5 imported into
`replaced_at`, and from then on `sdb_chat_archived_titles` (which decodes with the message range, 0 <= t < 2^63)
refused the row, so every export, backup and fork of the project failed.

**Change.** Title lines take the message lines' range (`chat_store_load_files`): a time outside it is refused and the
title kept with no time, which the import stamps with the chat's last change, as it does a line with no time. Nothing
is deleted: every title is archived, the out-of-range stamp stays on its line in the file (set aside under legacy-*
by the import), and only the unusable time is not carried into the store. Clamping was rejected: a clamped time would
be a made-up time presented as the record's.

**Test.** `test_db` `chat_title_bad_time`: a chat with title lines stamped 1e300, -5 and 1700000000 reads as four
titles, the first two timed lines without a time; it imports with three archived rows, and its export succeeds with
all four titles in order. Before the fix the same test failed three checks (the export among them).
