# Observer cycle 1: a headless replay, and the finding it found

**Date:** 2026-11-12. **Tree:** `ece3439` plus the changes recorded below.
**Direction:** "Let's wrap up the loose ends. 100/100. Methodically." The
loose end is TECHNICAL-DEBT's worker-monolith entry (AUDIT-003 F5): `run_job`
in `observer.c` (285 lines) and `observer_job_find` in `observer_job.c` (234
lines) are to be split the way `build_in_worker` was
(`journal/answer-cycle4_builder.md`). A split is measured before it ships, and
the observer had no yardstick: its end-to-end tests run a few paragraphs with
the fake backend, and a live run with the model is not repeatable. This
journal builds the yardstick, and records what the yardstick found before any
split was made.

## The replay

`tools/observer_replay.c` (`make tools`, `build/observer_replay`) runs the
observer headless over one project folder with the deterministic fake backend
(no model server, no semantic index), as `test_observer_run` does, until the
observer reports "Up to date"; then it lists every finding it recorded, one
line each (type, subject, relation, object, status, content, source kind and
name, range, quote, related text), sorted, without the timestamped ids. Seeds
before the run cover the paths a bare manuscript does not reach: `--canon`
and `--note` append committed canon and active notes (the conflict and overlap
paths), `--chat` appends author messages to one chat (the chat job).
`tools/observer_replay_run.sh <label> <binary>` runs it over copies of the
three test manuscripts (the first novel's eight parts, the second's 28
chapters, the third's 34) with fixed seeds, in parallel, and writes
`build/observer/<label>-{pp,mars,heights}.txt`. Two binaries built from two
trees are compared byte for byte on the three outputs.

The seeds are shaped for the fake backend's rules: a canon entry
"Wickham’s circumstances are fine." against the text's "Wickham’s
circumstances are not so hopeless…" (the fake judge's `<Name>'s <attr> is|are
<value>` form, so the conflict path writes a conflict), a note sharing three
words with that sentence (the overlap path), and chat questions and names (the
chat job writes a question finding).

## What the first baseline found

Two runs of the same binary on the same copies differed: the first novel's
run lost "Don’t is mentioned" and one question in one run and not the other;
the second novel's gained two entities in one run. The fake backend is
deterministic and the jobs count was identical, so the findings were written
and then hidden. The logs say how: `findings.jsonl` had 716 lines in both
runs, and 714 distinct ids in one of them. **`records_new_id` draws a
16-bit random suffix behind a one-second stamp, independently per call, and
`records_create_ex` writes without reading the log** (the red-team 005
performance fix). The observer with the fake backend creates about ninety
findings a second; two ids collided in two of six runs, and the later finding
became version 2 of the earlier record, which from then on showed the later
text and hid the earlier one. With the model the rate is lower by two orders,
but a job writes up to eight findings in the same second, and nothing checked.

## The fix

`records_new_id` keeps its format (`f-YYYYMMDD-HHMMSS-xxxx`, which the stores
and the UI validate) and makes the suffix unique within the process for each
second: a random start when the second changes, then a count, under a mutex.
Across processes the findings lock serialises writers of a project's findings,
and `run_job` re-reads the log under that lock before it creates, so a second
app's ids are seen. `test_llm: records` draws 5,000 finding ids in a tight
loop and checks them distinct and well formed.

With the fix, three runs of `tools/observer_replay_run.sh` on the three
novels are byte-identical (`build/observer/base1-*`, `base2-*`, `base3-*`,
`observer_replay-idfix`):

| novel | findings | jobs | conflicts | overlaps | chat-sourced | seconds |
|---|---|---|---|---|---|---|
| first (8 parts) | 717 | 269 | 1 | 1 | 3 | 9 |
| second (28 chapters) | 348 | 144 | 0 | 0 | 1 | 4 |
| third (34 chapters) | 948 | 261 | 0 | 0 | 1 | 8 |

The duplicate-id count in every log is 0. This is the baseline the two splits
are measured against.

## What stays open

A findings log written before this fix may hold a collided id: a record whose
versions have different content and different sources, which a status change
never produces. Such a record shows the later finding and hides the earlier.
Detecting it is a scan of each version pair; repairing it means giving the
hidden version its own id, which is a migration of the author's record and the
author's call (MEMORY-015: delete nothing). Recorded in TECHNICAL-DEBT; the
local test databases are disposable and no author project on this Mac is known
to be affected, but none has been scanned.

## Gates

`make test` 95,963 C checks and 174 Python tests, 0 failures (one check more
than `ece3439`: the id loop); `make analyze` 0 warnings across 87 source files;
`make sanitize` 95,963, 0, 0 diagnostics; `make tsan` 48,593, 0, 0
diagnostics; `make dbtest` 18,000, 0. Every exit status 0
(`build/observer/idfix-gates-*.log`, `idfix-gates-exit.txt`).

## Hosted verdicts

CI and Trusted acceptance both passed on `2fd2866` (runs 43320587825 and
43320587697).
