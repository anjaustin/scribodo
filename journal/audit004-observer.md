# AUDIT-004 remediation: the observer stream

2026-11-15. Findings F2, F4 (observer part), F5, F7 and F12 (observer bits) of `docs/AUDIT-004-repository-audit.md`,
in the worktree `build/wt-observer` (branch `remed/observer`, from `ed95681`). One commit per item. After each, on
the committed tree: `make test`, `make analyze`, `make sanitize` (exit statuses in
`build/wt-observer/build/audit004-gates/<item>-exit.txt`, logs beside them) and the fake-path yardstick.

**Yardstick.** Before any edit, the unchanged tree's `observer_replay` was copied to `build/observer_replay.base` and
run as `tools/observer_replay_run.sh base` (flags unset): 717 / 348 / 948 finding lines on pp / mars / heights. The
same binary was run under each observer flag for the arms' references: `base-gate`
(`SCRIBODO_OBSERVER_CONFLICT_GATE=1`, identical to `base` on all three), `base-sent` (`SCRIBODO_OBSERVER_SENTENCES=1`,
step 9's arm: 716 / 348 / 948, the seeded Wickham conflict dropped as step 8 recorded) and `base-both`. Step 8's own
flag-on run from its commit is `build/observer/senton-*` in the main checkout. `build/audit004-gates/yard.sh <label>`
builds the replay and compares, per novel, flag off against `base`, `=attributes` against step 8's `senton`,
`=subject` against `base-sent` and the gate against `base-gate`. Unchanged-tree `make test`: exit 0, 78 s.

Gating: from item 1 on, each commit was checked out (detached) in a second worktree, `build/wt-observer/build/gatetree`
(submodules and the MariaDB client build linked from this worktree), and `build/audit004-gates/gate-commit.sh <item>
<sha>` ran the three gates there on exactly the committed tree, then the yardstick from that tree's replay: flag off
against `base`, each arm against its reference, and both flags on (`=1`) against `base-both`. The gate worktree was removed
at the end; its runs' `.txt`, `.err` and flag files are kept in `build/wt-observer/build/audit004-gates/yard-runs/`. The tallies below are
the sums of the per-binary "N checks, M failures" lines and of the Python "Ran N tests" lines. One mistake on the way,
recorded: while stopping my own first in-worktree gate run I ran `pkill -f "clang --analyze"`, which matches any
user's analyzer process; if another stream's `make analyze` was running at that moment (16:5x) it was killed and
should be rerun. Every later stop was by PID.

## Item 1, F2a: step 8's arm selectable again (`fba538c`)

`SCRIBODO_OBSERVER_SENTENCES` now has one level per measured arm: `=attributes` is step 8 (sentence pairing with the
attribute co-signer; *Mars* 6 true of 42 shown, *P&P* 5 of 18, end to end), `=subject` is step 9 (and the
same-subject co-signer; 4 of 5 and 5 of 7, scored offline on step 8's recorded replies), and `=1` keeps naming the
latest measured arm, `subject`, which is what it selected when step 9 landed (`12ed760`), so the runs and scripts
recorded since keep their meaning. Anything else is off. `Experiments.observer_sentences` is an enum whose OFF is 0.

**Decision, levels of one variable rather than a second flag:** step 9 adds a co-signer to step 8's mechanism; a
separate subject flag could name "subject check without sentence pairing", which no one measured and which does
nothing. The field comment cites both steps with their numbers. Both co-signers are still computed and written to
the judge log on every sentence call whatever the level, so one run can be scored under either rule; the findings are
the arm's.

Tests: `test_core` `observer_sentence_levels` (the eight values parse to their levels); `test_llm`
`observer_sentence_arms`: a document whose accented name's colour conflict (*Zoë*) is recorded by `=attributes` and
not by `=subject` or `=1` (the ASCII-only subject rule, step 9's known limit), and whose *Sola* conflict every arm
records. Yardstick: off byte-identical with `base` (717 / 348 / 948); `=attributes` byte-identical with step 8's own
flag-on run (`build/observer/senton-*`, from `b492605`'s tree); `=subject` identical with `base-sent`.

Gates (`item1-exit.txt`): `make test` exit 0, 95,844 C checks and 200 Python tests, 0 failures; `make analyze` exit 0,
0 warnings across 87 source files; `make sanitize` exit 0, 95,844 checks, 0 failures; yardstick 15 of 15 identical.

## Item 2, F2b: one fixture held by the C and the Python (`dd8e771`)

`tests/fixtures/observer_rules/cases.json`: 16 sentence cases, 24 attribute cases, 19 subject cases (titles,
initials, straight and curly closing quotes, an opening quote or bracket before the capital, em dashes spaced and not,
possessives with straight and curly apostrophes, accented names and capitals, digits, an ellipsis, a no-break space,
leading spaces, more than 64 numbers, sets that differ only past the 64th word, a 70-digit number, a 70-letter place
and name). Read by `test_observer_text.c` (`shared_rule_fixture`) through `obs_next_sentence`,
`obs_attributes_differ` and `obs_same_subject`, and by `tests/test_observer_rules.py` through
`observer_sentence_pairs.sentences` (BOUNDARY with `observer_plant.ends_sentence`), `observer_judge_score.a_any` and
`same_subject`; both in `make test`. Run against the pre-change code the fixture fails 9 cases in C and 3 in Python.

Divergences found and how each was resolved (the rule: where the Python is the frozen definition the C was ported
from, the attribute and subject rules, the C follows it; where the C is what step 8 measured end to end, the sentence
loop, the Python follows it):

| divergence | resolved |
|---|---|
| C word sets: 64 words of under 64 bytes (`ATTR_MAX`/`ATTR_LEN`), the Python unbounded | C sets unbounded (growing arrays; a vocabulary word is added as the list's own entry, so no fixed buffer remains); an allocation failure answers "cannot check" as before |
| C `\b` before of/in/from/at/to read any non-ASCII byte as a letter, so an em dash or a curly quote hid a place | C uses `py_word_at`, as the possessive rule already did |
| C `\s` after the preposition was ASCII only | C reads Python's whitespace set (enumerated from `re`) |
| Python `\d` read Unicode digits | Python reads `[0-9]`, as the C |
| Python split sentences at any Unicode space and kept a line's leading spaces | Python follows the observer's loop: space or tab only, leading spaces dropped |

The observer's sentence loop moved out of `job_judge_sentences` into `obs_next_sentence` (observer_text.c), now also
used by `observer_overlap --sentences`, so the fixture tests the product's own loop. The comment at
`observer_plant.py` `ends_sentence` now says what differs (the C also needs BOUNDARY's following capital).

Nothing measured moves: `observer_judge_score.py`'s output on the step 6 and step 8 judge logs (both novels) is
byte-identical before and after; over all 4,782 recorded judge calls the new C and the new Python agree on both rules
and the new C equals every one of the 2,244 logged attribute answers; over the three novels (corrected planted Mars
and P&P, Wuthering Heights; 5,220 lines, 14,566 sentences) the C and the Python split identically, and the new Python
splits exactly as the old one did.

Gates (`item2-exit.txt`): `make test` exit 0, 95,965 C checks and 205 Python tests, 0 failures; `make analyze` exit 0,
0 warnings across 87 files; `make sanitize` exit 0, 95,965, 0; yardstick 15 of 15 identical.

## Item 3, F7: versioned judge log and finding lines (`ee6d2d6`)

`src/llm/observer_formats.h` defines both formats once, in the receipt contract's pattern, with
`observer_formats.c` holding the two formatters (moved out of `observer.c` and `tools/observer_replay.c`). The judge
log's first member is `"version":1`; a log without it is version 0 (the same members), and
`observer_judge_score.load()` refuses any other. The finding lines are format 2 (escaped, twelve fields); the marker
`# observer-findings format 2` goes to the replay's **stderr**, not stdout, so stdout stays byte-comparable with every
earlier run (the yardstick); `observer_lines.read()` looks for it in the `.err` beside the `.txt`, refuses an unknown
declared format and reports the declared one (None for older runs) in `stats["format"]`.

`tests/test_observer_formats.c` writes `tests/fixtures/observer_formats/` (three judge-log lines: a document statement,
a sentence call whose co-signer answers the C computes, a chat call with a null second reply and escapes in its
texts; three finding lines with escaped newlines, carriage returns, tabs, pipes, backslashes and UTF-8, one without a
source; the stderr marker; the fields as JSON) and compares byte for byte (`SCRIBODO_WRITE_FIXTURES=1` rewrites).
`tests/test_observer_formats.py` reads them with the tools' readers: members and order, the logged co-signers equal
to the Python's answers, labelling and scoring, version 0 accepted and version 2 refused, every finding field
unescaped exactly, an unknown declared format refused. The live judge-log test in `test_llm` checks every line's
version.

Gates (`item3-exit.txt`): `make test` exit 0, 95,989 C checks and 213 Python tests, 0 failures; `make analyze` exit 0,
0 warnings across 88 files; `make sanitize` exit 0, 95,989, 0; yardstick 15 of 15 identical.

## Item 4, F4: every flag combination tested, the gate's title rule scoped, runs pinned (`840d211`)

**Decision on `tokens()`.** The conflict gate's title stoplist (step 5) was built and measured as a rule of the canon
candidate step: "the canon candidate step ignoring a shared title". Living in `tokens()`, it also changed note linking
(`job_note_overlap`) and, later, sentence pairing (`sentence_canon`). Note linking was never part of what step 5
measured, and its runs seeded no notes (`real-conflict.log`: `canon=20`, no `--note`), so it is not intended there:
`obs_overlap` now takes `ignore_titles`, the observer passes the gate to its two canon candidate steps and `false` to
note linking. Sentence pairing keeps the gate, because choosing canon for a sentence is a canon candidate step; that
combination was never measured either way and its disclosure says so (item 6). Step 5's arm is reproducible: its
canon path is unchanged, and the gate yardstick stays byte-identical with `base-gate`. This also removes the text
module's read of the flag table (F12).

Tests: `test_llm` `observer_flag_combinations` runs all six arms (gate off/on x sentences off/attributes/subject)
through the fake backend and checks the conflicts and which, which path judged, whether the canon entry that shares
only "Captain" and "horse" was judged (by default yes, under the gate never), and that the note sharing the same two
words is linked in every arm; with the note scoping reverted three arms fail. `test_observer_text` passes the
setting explicitly and shows the flag in the environment no longer matters to it.

`tools/observer_replay_run.sh` pins `SCRIBODO_OBSERVER_CONFLICT_GATE` and `SCRIBODO_OBSERVER_SENTENCES` (default off),
unsets the test hook, prints them and writes `build/observer/<label>-flags.txt`; the replay writes the flags it read on
each `.err`.

Gates (`item4-exit.txt`): `make test` exit 0, 96,045 C checks and 213 Python tests, 0 failures; `make analyze` exit 0,
0 warnings across 88 files; `make sanitize` exit 0, 96,045, 0; yardstick 15 of 15 identical (the gate arm with
`base-gate`, both flags with `base-both`).

## Item 5, F12 observer bits (`861f6f2`)

- **`tokens()` and the flag table:** done in item 4 (`obs_overlap`'s parameter); `observer_text.c` no longer includes
  `core/experiments.h`.
- **`MAX_SENTENCE_PAIRS`:** its comment states the source: set when step 8 was built (`b492605`), four times
  `MAX_JUDGE_PAIRS`, with no measurement behind it, and it never bound a measured job (the step 8 runs'
  `on-{mars,pp}.err` print no "sentence pairs past" line over 670 and 1,574 judge calls). Kept at 96, since changing
  it changes the arm. It no longer sizes every job's conflicts: `JobRun.conflicts` is allocated per job at the path's
  cap (24 on the statement path, 96 on the sentence path; a conflict comes from one judged pair, and each path stops
  judging at its cap), and an allocation failure leaves the text to be read again.
- **`SCRIBODO_FAKE_EXTRACT_MS`:** now `Experiments.fake_extract_ms`, under a "test hooks, not experiments" heading in
  `experiments.h`, read once with the flags; `observer_fake.c` reads the table, not `getenv`, per extraction. The wait
  mode test reloads the table around it; `test_core` pins the parsing (positive counts only).
- **Null dereferences in `test_llm`:** the judge-log and sentence-pairing tests read members through `json_str`
  ("" when missing) and `cJSON_GetNumberValue`.
- **`worker` split:** `worker_wait` (45 lines), `worker_rebuild` (18), `worker_prepare` (46: state, renames, names,
  rules re-check), `worker_nothing_to_read` (15) and `worker_job_done` (35) under a 35-line loop; the statements, their
  order and where the lock is taken and released are the loop's. The observer tests, wait mode included, pass and the
  yardstick is byte-identical on every arm.
- **Fault fixtures:** `tests/fixtures/observer_faults/{mars,pp}.json` keep `"written"` and gain `"amended"`: the anchors
  of M7, M9 (closing quotes) and P4, P5, P8 (cut at "Mr.") were extended to their sentence's end on 2026-11-15
  (`4c6ea3f`), inserts, canon and facts unchanged, the other fifteen planting byte-identically; a harness defect found by
  `ends_sentence`, journal step 7.

Gates (`item5-exit.txt`): `make test` exit 0, 96,051 C checks and 213 Python tests, 0 failures; `make analyze` exit 0,
0 warnings across 88 files; `make sanitize` exit 0, 96,051, 0; yardstick 15 of 15 identical. `make tools` builds
(exit 0, no warnings).

## Item 6, F5: the disclosure follows the arm (`393e8cb`)

`src/llm/observer_arms.c` holds one row per arm (conflict gate off/on x sentences off/attributes/subject), each with
the sentence shown beside possible conflicts and its figure's source (conflicts on a planted fault over conflicts
shown, both planted novels):

| arm | figure | disclosure says | source |
|---|---|---|---|
| default | 7 of 92 | about 1 in 13 (the text since step 4, unchanged) | step 4, end to end |
| conflict gate | 5 of 149 (Mars 3/64, P&P 2/85) | about 1 in 30 | step 5, end to end |
| sentences, attributes | 11 of 60 (6/42, 5/18) | about 1 in 5 | step 8, end to end |
| sentences, subject (and `=1`) | 9 of 12 (4/5, 5/7) | about 3 in 4, "scored on recorded replies" | step 9, offline on step 8's replies |
| gate with either sentence level | none | "this setting was not measured on the test novels" | not measured |

The unmeasured arms say so rather than borrow a figure. `findings_view.c` reads `observer_arm(experiments())`.
`test_observer_text` `observer_arm_table`: every combination has exactly one row with a disclosure and a source (a
step, or "not measured"), the default text is the step 4 sentence byte for byte, and the row follows the flags. The
replay writes the arm and its figure on stderr, so a run's `.err` names what it measured.

Gates (`item6-exit.txt`): `make test` exit 0, 96,079 C checks and 213 Python tests, 0 failures; `make analyze` exit 0,
0 warnings across 89 files; `make sanitize` exit 0, 96,079, 0; yardstick 15 of 15 identical.

This journal's commit changes no code: its tree differs from `393e8cb`'s only in this file, so the item 6 gates are
the tip's.

## Not done here, and README rows

Nothing in the six items was left out. Outside the scope: the other `cJSON_GetObjectItem(...)->valuestring` uses in
`test_llm.c` (chat tests, lines ~460-525, 772-776, 1110, 2545) have the same shape as the two fixed here; the
remaining F12 bullets (records ids, `setenv` after threads, drafts, script host, `answer_pipeline.c`) belong to other
streams. No real-model run was made; the step 9 arm is still measured only offline.

Rows proposed for the README's flag table (the coordinator writes it): `SCRIBODO_OBSERVER_SENTENCES` =
`attributes` | `subject` | `1` (= `subject`), default off, steps 8 and 9; `SCRIBODO_OBSERVER_CONFLICT_GATE=1`, default
off, step 5 (now the canon candidate steps only, not note linking); `SCRIBODO_FAKE_EXTRACT_MS` test hook (never in a
measurement).
