# Red team: the loose-end commits, and what it found

**Date:** 2026-11-12. **Tree:** `a59a7ff` plus the changes recorded below.
**Direction:** "Let's red-team the changes and remediate. Let's keep the pressure
on." The changes are the day's five code-bearing commits: the in-word emphasis
marker (`5f9b53f`), the inheritance relation rule (`efb391e`), the finding-id
generator and the headless replay (`2fd2866`), the observer worker split
(`2aede34`), and the third novel's reach fixture (`f2558ea`, data only).

## Method

Each diff read adversarially against the function it replaced, then probed
where reading is not proof:

- **A differential fuzz of the normalization.** `answer_normalize` (the C
  function the span verifier, the claim gate, the quotation check and the
  relation check share) against its Python mirror `normalize` in
  `tools/answer_citation_binding.py` (which the relation arm and the
  citation-binding classifier use): 200,000 random strings per seed over an
  alphabet of letters, digits, the two markers, hyphen, the quotes and dashes
  in both spellings, spaces of several kinds, an accented letter and an
  ellipsis, through a probe binary linked against the product's objects.
- **A differential probe of the relation check.** 26 synthetic statements
  around the new inheritance rule (tense, negation in the source, honorifics,
  case, a plural, the "of" form, a name with a lowercase particle) through
  `answer_relations` and the arm's `check`.
- **The replay yardstick** for anything touching the observer
  (`tools/observer_replay_run.sh`, byte-identical findings on three novels).
- **The tolerance tool** for anything touching the normalization
  (`tools/answer_span_tolerance.py`, 681 recorded spans).

## Findings

| # | where | finding | disposition |
|---|---|---|---|
| 1 | normalization | **The C normalization and its Python mirror disagreed on 46,955 of 200,029 random strings**, every one containing a no-break space (U+00A0): Python's `\s` is Unicode whitespace, C's `isspace` is ASCII. Behind the mirror defect sat a product gap: a manuscript's no-break space against a model's plain space is a rendering difference the verifier rejected (the tolerance tool, with a new mutation: 681 of 681 rejected). | Remediated below: Unicode spaces are blanks; the mirror is a port of the C loop; fuzz 0 of 600,000. |
| 2 | normalization mirror | The port also exposed two smaller mirror-only differences the regular expressions had: `a'*b` folded to `ab` in C (the quote looks at the next raw byte, a marker) and `a'b` in Python (whose look-around saw the string after the markers were gone); and Python lower-cased non-ASCII letters, C does not. | Gone with the port; pinned. |
| 3 | finding ids | **The generator's comment claimed cross-process safety it did not have.** The per-second count makes one process's ids distinct; across two processes, or against a log written before the count, `records_create_ex` still writes without reading and nothing checked the id. In the database store the primary key on (id, version) rejects the write and the job is retried; in the file store the later finding becomes the next version of the earlier one and hides it. | Remediated below: the observer draws each id against the findings it re-read under the lock. |
| 4 | relation check | **A side effect of the inheritance rule:** "Mr. Bingley's inheritance was Netherfield Park" now passes. The possessive is supported by "inherited" and the whole statement is checked only in the present tense (patterns 3 and 4 know "is"). Before the rule it was flagged, by accident of the possessive. Measured on the 230 recorded replies: 11 relation statements in the past tense, every one already flagged by its possessive phrase, so no recorded verdict depends on it. | Remediated below: "is" and "was" alike, C and arm; zero verdicts change. |
| 5 | observer split | `run_job` and `observer_job_find` read stage by stage against the originals: the lock order, the generation checks, the `io_failed` paths (a list that cannot be read, a backend that returns nothing leaves `io_out` untouched as before, a chat whose unread messages are all skipped is marked read) and the frees are the originals'. | No change; the replay had already shown byte-identical findings. |
| 6 | replay tool | `records_list_checked`'s result is not checked before the findings are printed; the finding fields it prints are fixed-size arrays and never NULL. | Diagnostic; no change. |
| 7 | normalization, recorded then fixed | The relation check's sentence and clause splitting, the prefix cut in the verifier and the sentence splitter of the question use ASCII `isspace`, so a Unicode space after a period is not a sentence end there; and case folds for ASCII letters only. Measured: the three manuscripts and the 38 recorded runs contain no Unicode space and no non-ASCII letter at all, so no recorded case depends on either. | The whitespace half fixed in section 4 on the second day: two of the sites weakened a gate (a no-break-space-padded span passed the length floor; a question ending in one escaped the gate). The case fold stays ASCII in C and mirror alike. |

## 1. Unicode spaces are blanks, and the mirror is a port

`unicode_space_at` in `src/llm/answer_pipeline.c` reads U+00A0, U+2000 to
U+200A, U+2028, U+2029, U+202F, U+205F and U+3000 as a blank, as `isspace` and
the dashes already were; a zero-width space (U+200B) is not whitespace and is
kept. `normalize` in `tools/answer_citation_binding.py` is now a
character-for-character port of the C loop (the same alphabet of word
characters, the same blanks, the same marker and apostrophe rules, ASCII case
folding) instead of a chain of regular expressions that approximated it.

Measured, before the change (`build/tolerance/nbsp-before.txt`) and after
(`nbsp-after.txt`), with the tool's new orthographic mutation "no-break
spaces" (every space in the span becomes U+00A0):

| mutation | applied | before | after |
|---|---|---|---|
| no-break spaces | 681 | 0 verified, 681 rejected | 681 verified |
| every other row (18 mutations, 6,606 applications) | | identical | identical |

The 681 unchanged spans return the same bytes and statuses from both binaries
(`nbsp-unchanged-before.jsonl` equals `nbsp-unchanged-after.jsonl`); the gate
replay over the recorded claims and replies is byte-identical
(`nbsp-gate-before.txt`/`.json` equal `nbsp-gate-after.*`). The fuzz:
0 differences in 3 seeds of 200,029 strings each, the alphabet widened to the
thin, narrow no-break and ideographic spaces, a C1 control, a zero-width space,
a capital accented letter, and \r and \v. No manuscript or recorded reply in
the evidence holds a Unicode space, so no recorded case moves; the gap is the
one a manuscript from a word processor brings.

Pins: `test_normalize` (C) and `test_normalize_mirrors_the_c_normalization`
(Python, the same strings byte for byte, plus `a'*b` and `ÉCOLE`);
`test_answer_span_tolerance` pins the new mutation.

## Gates (section 1)

`make test` 95,964 C checks and 176 Python tests, 0 failures (one C check and two
Python tests more than `a59a7ff`); `make analyze` 0 warnings across 87 source
files; `make sanitize` 95,964, 0, 0 diagnostics; `make tsan` 48,594, 0, 0
diagnostics; `make dbtest` 18,000, 0. Every exit status 0
(`build/tolerance/nbsp-gates-*.log`, `nbsp-gates-exit.txt`).

## 2. A finding id is drawn against the log just read

`records_new_id_unused` (`src/app/records.c`) draws with `records_new_id` and
moves on while the id is one of a list of records; `job_create` in the observer
passes the findings it re-read under the write lock (`job_revalidate`), so the
id it writes with `records_create_ex` (which writes without reading, red-team
005 #12) is never one the log already holds: not another process's, not one
from a log written before the per-second count. Within the job the count keeps
the ids apart as before. The cost is a string comparison per stored finding per
new id (24 ids at most per job; the comparison fails on the stamp's first
differing byte). The generator's comment no longer claims what the lock alone
did not give.

Measured: the replay yardstick is byte-identical to the baseline on the three
novels (`build/observer/idcheck-{pp,mars,heights}.txt` equal `base1-*`); the id
check changes no finding when no id is taken. `test_llm: records` draws an id,
lists the next three as taken and checks the fourth is drawn, well formed, and
in the same second exactly four past the first.

The database store was already safe by a different route: `record_versions` has
a primary key on (record_id, ver), so a reused id fails the insert, the job
reports `io_failed` and the text is read again (red-team 008). The file store
had no such key; it has the check now. A log written before the fix may still
hold a collided id; that entry stays in TECHNICAL-DEBT (the repair is the
author's).

## Gates (section 2)

`make test` 95,967 C checks and 176 Python tests, 0 failures (three C checks
more than `a6a216a`: the taken-id draw); `make analyze` 0 warnings across 87
source files; `make sanitize` 95,967, 0, 0 diagnostics; `make tsan` 48,597, 0,
0 diagnostics; `make dbtest` 18,000, 0. Every exit status 0
(`build/observer/idcheck-gates-*.log`, `idcheck-gates-exit.txt`).

## 3. A relation stated in the past tense is checked whole

Patterns 3 and 4 of the relation check ("X's KIN is Y", "X is the KIN of Y")
accepted "is" only, so "Mr. Bingley's inheritance was Netherfield Park" was
checked as the possessive alone, which the inheritance rule now supports. Both
patterns read "is" and "was" alike, in C (`rel_match_at`) and in the arm (the
regular expressions). Measured with the arm over the 24 capture directories,
before (`build/relation/arm-tense-before.json`) and after (`arm-tense.json`):

| | before | after |
|---|---|---|
| replies flagged | 21 of 230 | 21 of 230 |
| kinship phrases supported / negated / flagged | 66 / 10 / 23 | 66 / 10 / 23 |
| possession phrases supported / negated / flagged | 13 / 8 / 37 | 13 / 8 / 44 |

The seven new phrases are "Mr. Bingley's estate was Netherfield", checked
whole in the seven first-novel runs that wrote it; each of those replies was
already flagged for "Mr. Bingley's estate", so no verdict moves. The parity
fixture's seven rows carry the new count (`checked` 7 to 8); the 230 rows,
the 21 flagged and the unsupported phrase of every row are otherwise
unchanged, and the C check agrees with the arm on every row (`test_relations`).
The probe's cases are pinned in `test_relations` and
`test_answer_relation_arm`: "Mr. Bingley's inheritance was Netherfield Park"
is flagged whole, "Mr. Bingley was the heir of Mr. Darcy" too (and its "heir
of Mr. Darcy" by pattern 1, as before).

## Gates (section 3)

`make test` 95,968 C checks and 176 Python tests, 0 failures (one C check more
than `c979df5`); `make analyze` 0 warnings across 87 source files; `make
sanitize` 95,968, 0, 0 diagnostics; `make tsan` 48,598, 0, 0 diagnostics;
`make dbtest` 18,000, 0. Every exit status 0 (`build/relation/tense-gates-*.log`,
`tense-gates-exit.txt`).

## 4. Unicode spaces are blanks for the scans too (2026-11-13)

Re-reading finding 7 before moving on: the Python mirrors already read a
Unicode space as whitespace (`\s`, `strip`), the C scans did not, so the
mirror and the product disagreed exactly where the normalization had just been
made to agree. Four sites read ASCII whitespace: `trimmed_length` and the
prefix cut in `answer_verify_span`, the trailing-blank scan in
`answer_gate_applies`, the sentence and clause scans behind `rel_prose`, and
the sentence and conjunction scans behind `answer_question_parts`. Each site's
consequence, measured with a probe linked against the previous commit's
`answer_pipeline.c` and against the new one (`build/tolerance/uspace-pins-old-new.txt`):

| Site | Input | Before (`d849de6`) | After |
|---|---|---|---|
| verifier length floor | `"\u00a0\u00a0 lucky idea \u00a0\u00a0"` (ten letters) | `verified`, span "lucky idea" shown | `too_short` |
| verifier prefix rule | `"…only three miles.\u00a0Then she took the carriage."` | `rejected_not_in_unit` | `verified_prefix`, the manuscript's sentence |
| gate applies | `"Why?\u00a0"`, `"Why?\u202f\n"` | gate off | gate on |
| relation check | `"Mr.\u00a0Bingley's\u00a0estate\u00a0is\u00a0large."` | 0 phrases found | 1, unsupported, "Mr. Bingley's estate" |
| question splitter | `"Who is Jane?\u00a0Who is Bingley?"` | 1 part | 2 parts |

The first and third rows are gate weakenings: a model that writes no-break
spaces could pass a span shorter than the floor, and a question pasted with a
trailing no-break space was not gated at all. The second and fifth are
tolerance the normalization already granted and the scans withheld; the
fourth is the relation check skipping a statement it checks when spaced
plainly.

The fix is one helper, `ascii_spaces`, a copy of the text with every Unicode
space of `unicode_space_at` written as a plain space (zeroed buffer: a
multi-byte space shrinks to one byte), taken at the entry of the four
functions; the ASCII scans then see the blank the normalization sees. The
arm's `prose` makes the same substitution (`UNICODE_SPACES`) so its literal
spaces match a no-break space as the C does; the question-parts mirror
already split on `\s`. Measured on the evidence, before and after,
byte-identical as the zero count of Unicode spaces predicts: the tolerance
table (`build/tolerance/uspace-before.txt`, `uspace-after.txt`, 681 spans,
nineteen mutations), the gate replay (`uspace-gate-before/after.txt|json`),
the relation arm over the 24 captures (`build/relation/arm-uspace-before.json`,
`arm-uspace-after.json`, 21 of 230 flagged, 66/10/23 and 13/8/44). The
relation probe agrees with the arm on 32 of 32 cases, six of them with
no-break, narrow no-break and ideographic spaces around "is", after a period,
before a clause's "not" and inside "but"; the normalizer fuzz, re-run because
the probes were relinked, is 0 of 200,029 (seed 11). Pins: `test_verify`,
`test_answer_gate`, `test_relations` and `test_question_parts` carry the
table's rows (each fails against the previous object, the probe's "before"
column); `test_answer_relation_arm` and `test_answer_question_parts` the
mirrors'. The first analyzer run flagged the helper's `malloc` tail as
possibly read uninitialized through `strlen`; the buffer is zeroed and the
gates were run again on the final tree.

## Gates (section 4)

`make test` TEST C checks and 176 Python tests, 0 failures (seven C checks
more than `bec575f`); `make analyze` 0 warnings across 87 source files (second
run, after the `calloc`); `make sanitize` TEST, 0, 0 diagnostics; `make tsan`
TSAN, 0, 0 diagnostics; `make dbtest` 18,000, 0. Every exit status 0
(`build/tolerance/uspace-gates-*.log`, `uspace-gates-exit.txt`).

## Hosted verdicts

CI and Trusted acceptance both passed on each of the three commits: `a6a216a`
(runs 43337258107 and 43337258238), `c979df5` (43338288483 and 43338288556)
and `bec575f` (43339299053 and 43339299187), and on the journal commit
`d849de6` (43340371114 and 43340371251).

## What stays open

- A findings log written before 2026-11-12 may hold a collided id
  (TECHNICAL-DEBT, open P2); the repair is the author's.
- Case folds for ASCII letters only, in the C and the mirror alike; the
  evidence holds no non-ASCII letter to measure it against (finding 7; its
  whitespace half is section 4). Python's `\s` also covers U+0085 and the
  C0 separators U+001C to U+001F, which `unicode_space_at` does not; neither
  appears in the evidence.
- The notes and canon an author creates draw ids with `records_new_id` and
  append through the log-reading path, one per click; a collision there needs
  two apps on one project in one second with the same random start, and is not
  checked.
