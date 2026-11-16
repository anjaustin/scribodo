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
same-subject co-signer; 4 of 5 and 5 of 7, scored offline on step 8's recorded replies), and `=1` kept naming
`subject`, what it selected when step 9 landed (`12ed760`). **Reversed after review (red-team 1, below):** `=1` is
`attributes`, because every real run ever made with `=1` was step 8's. Anything else is off (since red-team 2, with a
warning). `Experiments.observer_sentences` is an enum whose OFF is 0.

**Decision, levels of one variable rather than a second flag:** step 9 adds a co-signer to step 8's mechanism; a
separate subject flag could name "subject check without sentence pairing", which no one measured and which does
nothing. The field comment cites both steps with their numbers. Both co-signers are still computed and written to
the judge log on every sentence call whatever the level, so one run can be scored under either rule; the findings are
the arm's.

Tests: `test_core` `observer_sentence_levels` (the eight values parse to their levels); `test_llm`
`observer_sentence_arms`: a document whose accented name's colour conflict (*Zoë*) is recorded by `=attributes` and
not by `=subject` (the ASCII-only subject rule, step 9's known limit), and whose *Sola* conflict every arm
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
| conflict gate | 5 of 149 (Mars 3/64, P&P 2/85) | about 1 in 30 | step 5; both runs hit their time limit (Mars ch25 of 28, P&P part 7 of 8) |
| sentences, attributes (and `=1`) | 11 of 60 (6/42, 5/18) | about 1 in 6 (said 1 in 5 until red-team 5) | step 8, end to end |
| sentences, subject | 9 of 12 (4/5, 5/7) | about 3 in 4, "scored on recorded replies" | step 9, offline on step 8's replies |
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
`attributes` | `subject` | `1` (= `attributes`; scripts should name the level), default off, any other value off with a warning, steps 8 and 9; `SCRIBODO_OBSERVER_CONFLICT_GATE=1`, default
off, step 5 (now the canon candidate steps only, not note linking); `SCRIBODO_FAKE_EXTRACT_MS` test hook (never in a
measurement).

## Red-team remediation (2026-11-15, the coordinator's seven findings)

Gated as before, each commit checked out alone in `build/gatetree` (recreated) by `gate-queue.sh`; `make analyze` now
runs only when `pgrep -f "clang --analyze"` finds no other analyzer (waiting up to 30 minutes, else recorded as
skipped). The "both flags" yardstick names `subject` explicitly against `base-both` (the unchanged build's `=1`, which
was `subject` then).

1. **`=1` means `attributes`** (`7a8016f`, with 2). The only real-model runs ever made with `=1` were step 8's
   (`build/observer/sentence-cycle/chain.sh`, `chain2.sh`); `=1` meant `subject` for seven minutes (`b492605` 14:38 to
   `12ed760` 14:45) with no run between. Field comment updated; `observer_replay_run.sh`'s comment asks for level
   names. With `=1` the fake yardstick is byte-identical with step 8's own `senton` run on all three novels.
2. **Unrecognised values warn** (`7a8016f`). Decision: values match exactly, case-sensitive and untrimmed, like every
   flag in the table, so a run log's value is the value that counted; unset, empty, `0` and `off` are off silently;
   anything else (`Attributes`, `on`, `true`, `attributes `) is off with a warning on stderr, also returned by
   `experiments_warnings()`. Same for `SCRIBODO_OBSERVER_CONFLICT_GATE`. `observer_replay --print-flags` prints the
   parsed arm (exit 3 on a warning); the runner writes `given:` and `parsed:` lines to `<label>-flags.txt` and refuses
   to run (exit 2, nothing measured) on a warning. Tested in `test_core` (19 values) and by running the runner with
   `Attributes` (refused) and `1` (parsed `attributes`).
3. **Word sets O(n log n)** (`97b766d`). Append, then sort and drop repeats once; compare element-wise; intersect by
   merge. 40,000 a side: 3.7 s and 8.0 s before, 6 and 12 ms after; 100,000: 16 and 34 ms. Allocation failure still
   "cannot check". Bounded-time test: 100,000 a side under 2 s. Verdicts byte-identical before and after on 64,782
   pairs (all 4,782 recorded calls and 60,000 random sentence pairs from the three novels).
4. **Python's `\w` for every script** (`1924d63`). `py_word_at` decodes 1 to 4 bytes and looks the code point up in
   `observer_unicode_word.h`, generated from Python 3.9.6's `re` (Unicode 13.0.0, 703 ranges) by
   `tools/gen_unicode_word.py`; `test_observer_rules.py` checks the table against the running Python. Twelve fixture
   cases added (the previous C fails 7). Random differential with four scripts, digits, symbols, emoji, combining marks
   and Unicode spaces: 0 of 120,000 disagree (the previous C: 44 of 60,000). Corpus verdicts unchanged by this commit
   (same 64,782 pairs, byte-identical).
5. **Figures** (`a04fecb`). 11 of 60 is 0.183: "about 1 in 6". The gate's 5 of 149 says its runs stopped at their
   time limit. Tested.
6. **Wait-mode test** (`0c301a9`). The hook is set and the table reloaded before `observer_create`, unset and
   reloaded after `observer_destroy`; no reload while the observer thread runs.
7. **The attributes arm's definition changed in item 2** (the boundary before of/in/from/at/to now reads an em dash or
   curly quote as a non-letter, and Unicode spaces after it as spaces; item 4 then extended that to every script).
   Recorded, as the arm changed: the coordinator's review counted 8 Wuthering Heights sentence pairs and 2 recorded
   statement pairs whose attribute check changed, none a conflict, Mars and P&P unchanged. My reproduction against
   `ed95681`'s C: the 2 recorded statement pairs exactly (both P&P, *"Meryton — A place where..."* and *"Rosings — A
   location from which..."*, 0 to 1, the judge's verdict on both "no conflict", so no recorded finding changes); 0 of
   the arm's own sentence pairs on Mars (670) and P&P (1,576, the step 8 runs' 670 and 1,574 calls) and 0 of all
   23,860 and 59,080 sentence-canon pairs; on Wuthering Heights exactly one sentence's place reading changes (*"... a
   mass of correspondence—daily almost ... —from ..."*, an em dash before "from"), which changes no verdict against
   the replay's two canon seeds, so the 8 pairs depend on the canon it is paired with; I could not reproduce that
   count without the review's pairing. The subject check changed on none. Step 8's measured numbers (6 of 42, 5 of 18)
   were taken with the old definition; on its novels the new one gives the same verdicts on every judged pair.

8. **A single capital is an initial only in context** (`f2d1e0f`, added by the coordinator after the data/text
   stream's probe: `build/audit004-text/redteam/observer-probe.txt` in the main checkout). `obs_sentence_end` and
   `obs_first_sentence` (the latter on the default path) took every single capital before a period for an initial, so
   "in Chapter V. Lydia is", "in Part I. That is" and "said I. \"Proud" were each one sentence. `name_initial` in
   `observer_text.c` is a copy of the data/text stream's `text_name_initial_before` (`src/core/text_rules.c` on
   `remed/text`, not merged, not depended on) with its two lists: not "I", the word before capitalised and not a label,
   the word after capitalised and not a sentence starter. The Python mirror is `observer_plant.name_initial`, called
   by `ends_sentence`, which now takes the rest of the line as `after` (`sentences()` passes it; `plan()` passes 200
   characters). The fixture gains the three probe sentences, two initial cases that must not split, and the rule's
   known cost ("I met a K. Smith there." splits after "K."); its "Mr. X. Then I left." case, pinned in item 2 as a
   known limit, now splits. Measured before committing:
   - fake yardstick flag off: byte-identical, 717 / 348 / 948, no line changed (the fake judge's explanations hold no
     initials); every flag-on arm identical with its reference, both flags on identical with `base-both`;
   - C sentences, old to new: *Mars* 2,386 to 2,386, *P&P* 5,908 to 5,911, *Wuthering Heights* 6,272 to 6,290 (14,566 to
     14,587); the C and the Python agree on every line. The new splits are the cases the rule is for: "... said I.
     \u201cProud people ...", "... T. G. It serves as a guide-post", "... exclude me from P. I shall never ...";
   - `obs_first_sentence` (the default path: a conflict's content) on every recorded real-model judge explanation
     (5,412, steps 6 and 8): 0 changed;
   - step 6 and step 8 judge-log rescoring: byte-identical; both planted novels plant byte-identically.
   So no default-path output changed on anything recorded.

Gates, each commit alone (`rt*-exit.txt`, `rt*-yard.txt`; analyze run only with no other analyzer present, which once
meant waiting for another stream's): every `make test`, `make analyze` and `make sanitize` exited 0, and every
yardstick was 15 of 15 identical.

| commit | C checks (test = sanitize) | Python tests | analyze |
|---|---|---|---|
| `7a8016f` (1, 2) | 96,110, 0 failures | 213 | 0 warnings, 89 files |
| `0c301a9` (6) | 96,110, 0 | 213 | 0, 89 |
| `97b766d` (3) | 96,116, 0 | 213 | 0, 89 |
| `1924d63` (4) | 96,140, 0 | 214 | 0, 89 |
| `a04fecb` (5) | 96,142, 0 | 214 | 0, 89 |
| `f2d1e0f` (8) | 96,156, 0 | 214 | 0, 89 |

The journal commit changes no code (its tree is `f2d1e0f`'s plus this file).

## Verification round (2026-11-15): item 8 reopened, a gap in item 4

**Item 8, second pass (`e2eb779`).** The verifier found the first rule cut names: it rejected an initial at offset 1
and required a capitalised, spaced word before it, so "I admire J. R. R. Tolkien", "\"J. Smith came,\"", P&P's
"\u201cE. GARDINER.\u201d" and "\u201cM. GARDINER.\u201d" and Wuthering Heights' "letters W. H." were split, and
`obs_first_sentence` (the default path) turned "Written by J. R. R. Tolkien." into "Written by J.", "T. S. Eliot wrote"
into "T." and "names E. Bennet" into "names E.", the step 3/4 truncation back again. My first-pass measurement did not
catch it: the novels' recorded explanations hold no such initials, and I counted splits without reading every new one.
Adopted the verifier's rule in C and in `observer_plant.name_initial`: a capital other than I, no word character
before it, the next word capitalised and not a sentence starter, the previous word (any case, past an opening quote,
or none) not a label. Fixture: Tolkien, Eliot, "names E. Bennet", "J. Smith", both GARDINER signatures and "W. H."
added; "I met a K. Smith there." is now one sentence; `test_observer_text` checks `obs_first_sentence` on Tolkien,
Eliot and E. Bennet. Measured:
- fake yardstick flag off byte-identical, 717 / 348 / 948; every arm and both flags identical with their references;
- C sentences against the first pass: *Mars* 2,386 to 2,386, *P&P* 5,911 to 5,909, *Wuthering Heights* 6,290 to 6,288.
  The four splits removed are exactly "\u201cE. GARDINER.\u201d", "\u201cM. GARDINER.\u201d", "W. H." and "T. G."
  (the carved letters "T. G. It serves", now cut only before "It"; the verifier listed three of the four). Against the
  tree before item 8: 14,566 to 14,583. The C and the Python agree on all 14,583;
- `obs_first_sentence` on the 5,412 recorded real-model explanations: 0 changed against both earlier versions;
- step 6 and step 8 rescoring byte-identical; both novels plant byte-identically.

**Item 4 gap (`9b1f991`).** The Python rules used the interpreter's `\b`, so under Homebrew's Python 3.12 (Unicode 15)
or CI's `python3` they read characters added after Unicode 13 unlike the C's table, while the table test skipped:
the contract was unchecked there. Of the coordinator's options I took a fourth that keeps the C table byte-stable and
checks the contract everywhere: `observer_judge_score.py` builds its word class from `src/llm/observer_unicode_word.h`
and uses table-defined lookarounds in the place and possessive rules, so both sides read one definition on any Python.
(`/usr/bin/python3` here is 3.9.6 / Unicode 13.0, the table's source, but CI's `macos-15` image cannot be checked
from this Mac, so pinning an interpreter would not have been a guarantee.) A test on every Python checks the scorer's
class is the table; the generator check still needs a matching Unicode version. Two U+0870 cases (a letter for 3.12,
not in the table) were added; the previous scorer gets both wrong under 3.12. `test_observer_rules.py` passes under
3.9.6 and 3.12; the random mixed-script differential is 0 of 60,000 under each; the rescoring is byte-identical under
both. The bounded-time test now measures `CLOCK_PROCESS_CPUTIME_ID`.

Gates, each commit alone (`rt8b-*`, `rt4b-*` in `build/audit004-gates/`): `e2eb779` `make test` exit 0, 96,173 C
checks and 214 Python tests, 0 failures; `make analyze` exit 0, 0 warnings across 89 files; `make sanitize` exit 0,
96,173, 0; yardstick 15 of 15 identical. `9b1f991`: `make test` exit 0, 96,177 and 215, 0 failures; `make analyze`
exit 0, 0 warnings across 89 files; `make sanitize` exit 0, 96,177, 0; yardstick 15 of 15 identical. No `git stash`. I never used `git stash` in this stream;
the tree held only the intended journal edit when checked after the coordinator's stash warning.
