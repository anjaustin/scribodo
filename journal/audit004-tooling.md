# AUDIT-004 remediation: tooling and evidence (2026-11-15)

The tooling stream of the AUDIT-004 remediation: F1 (evidence), F9 (tools no gate compiled), F4 (unpinned arms, the
scripts' part) and F12's tooling items. One commit per item on `remed/tooling`, based on `ed95681`. Every gate result
below is the exit status written into its log by the command that ran it (`echo "test exit=$?" >> LOG`), not text
read from the output; the logs are under `build/audit004-tooling/` in the stream's worktree and are archived in
`scribodo-evidence-2026-11-15-audit004-tooling` (see the end).

## 1. F1: `evidence_check` (`7d6c17f`)

`tools/evidence_check.py` reads every `build/` path cited in `docs/*.md`, `journal/*.md` and the README. A citation
starts at `build/` not preceded by a path or identifier character, so `rebuild/`, `$(BUILD)/` and `foo/build/` are
not citations; trailing punctuation and slashes are dropped, `{a,b}` braces and `ux1..3` ranges expand, and globs
and `<placeholders>` are families matched against archive members. Each path must be (a) archived, (b) in
`tests/fixtures/evidence_not_archived.txt` with the reason it is not evidence, or (c) in
`tests/fixtures/evidence_lost.txt` with the citing document. The check fails, listing every violation with its
`file:line`, and skips with a message where the archive folder is absent (hosted CI). `make test` runs it and its
21-case unit test (`make evidencecheck`, a separate `test:` prerequisite so its Makefile hunk stands alone).

Decision: membership is the tarball's own listing, not its manifest. Several manifests name a whole directory
(`build/product-memory-use`, `build/manuscripts`, `build/memory-reach/runs`) that later runs added to; a manifest
match would have passed every later run under them unarchived. A manifest counts only where its tarball is missing.
Listing all fifteen tarballs takes about a second.

**Finding.** On the tree as committed, 157 citations were neither archived nor listed, and 75 of them existed under
`build/`. The 2026-11-15-audit004 archive was described in `docs/EVIDENCE.md` as holding "the observer cycle's
real-model runs, judge logs and gate logs (`build/observer/*`)", the GAP-003 series, the native-services move and
REDTEAM-023's durability runs; its tarball holds no `build/observer` file at all, and 35 of `build/gap003`'s files.
`tools/evidence_check.py --unarchived` (every existing file under a cited path that no archive holds, binaries,
objects, `.dSYM` symbols and sockets left out) listed 6,130 files, 150 MB; they are
`scribodo-evidence-2026-11-15-audit004b.tar.gz` (40 MB, SHA-256
`cdd4079ee949c70ed77a5f7a648e75289710590c368efbf93a4a2256d10362c5`), with its manifest and `SHA256SUMS` entry.

The not-evidence list (43 entries at the end of the stream) is the app and tool binaries, the two locked environments, the
`BUILD=` and clean-checkout build trees named in prose (`build/sanitize`, `build/build`, `build/audit002-clean`,
`build/redteam017-mariadb-clean`, `build/redteam017-shallow`, `build/wt-ca908bd`), make's directories named in prose
(`build/unit`, `build/headless`, `build/test/DB`), the watchdog's file-name format and example, the gate output
directories, and frozen app copies inside archived runs. The lost list holds 19 entries: the eight AUDIT-004 named
as evidence (`build/ui/fit-on`, `build/ui/fit-native2`, `build/gap003/{gates,frames/gates,orphan/gates,reap/gates,soak-smoke}`
and its pattern form, `build/recover/gates2`) and pre-archive history (`build/cr1/score.json`, two bedrock event
files, the memory-continuity and memory-safety logs, `build/redteam2-*.log`, `build/remediation-*.log`,
`build/relation/rel4-before`). The `gates-*.log` files of the GAP-003 and recovery directories do exist and are in
audit004b; only the directory forms AUDIT-004 cites are missing. A lost path that reappears under `build/` fails
the check. After the archive: 656 citations, 567 archived, 40 lost, 49 not evidence, 0 violations.

Gates: `make test` exit 0, `make analyze` exit 0 (`item1-test.log`, `item1-analyze.log`).

## 2. F1: gates delete only what they own (`5968aaf`)

`make uitest` ran `rm -rf build/ui` before every run (`Makefile:330`), and that is how `build/ui/fit-on` and
`build/ui/fit-native2` were lost. It now deletes `build/ui/<script>` for each script it runs, as `uitest-real`
already did. `make clean` removed `build/ui` too; it now removes `UI_OWNED` (one directory per UI script, and the
store, backup, offline and remote suites') with `build/src`, `build/tests` and `build/asan`.
`evidence_check.gate_owned_dirs` lists the same directories plus `make cstest`'s volume (and, from item 3,
`build/mutationtest`). A concrete cited path under one of them must be archived or recorded lost; the not-evidence
list can excuse only an entry naming exactly that directory (the gate's own directory named in prose), never a glob
or parent. `tests/test_evidence_check.py` pins the list to the Makefile: it reads every `rm -rf` in every recipe but
`distclean` (expanding `$$name` over the scripts), the three suite scripts' `$PWD/build/ui/...` and `cstest.sh`'s
paths, and the output of `make -n clean`, and fails on a deleted path outside the list or on `build/ui` itself.
Reintroducing `rm -rf build/ui` in `uitest` fails it ("build/ui is deleted by a gate but not in
gate_owned_dirs"). `docs/TESTING.md` states the rule.

Verification without the display (uitest was not run): the recipe itself ran in a scratch copy of the Makefile,
`tools/watchdog.sh` and two scripts, with a stub app (`make uitest APP=<stub> -o <stub>`) and a
planted `fit-on/evidence.txt` and `smoke/stale.txt` under the copy's UI output directory: exit 0, `fit-on` kept, `smoke/stale.txt`
gone, both scripts' `work/` and `stderr.log` created; `make clean` there then left `build/ui/fit-on` in place.
Gates: `make test` exit 0, `make analyze` exit 0 (`item2-test.log`, `item2-analyze.log`). The first `make test` of
this item failed, correctly: the new TESTING.md paragraph names nine gate directories, which the check flagged
until they were listed by exact name.

## 3. F9: tools, CI, mutation controls (`65982ba`)

The six overlay-built tools are real rules in `make tools`: `build/memory-reach/memory_reach_eval`,
`build/memory-quality/memory_quality_eval`, `build/memory_native_eval` (without `chat.o`, as its overlay had it),
`build/memory-pipeline-soak/memory_pipeline_soak`, `build/chat_hang_probe` and `build/import_probe`. Their objects
compile through the ordinary `$(BUILD)/%.o` rule with `-MMD`, so a header change rebuilds them; the overlays relinked
every time instead. The hang probe moved from `build/hang-probe/` because that directory is an archived run
(2026-11-08). None needs llama-server or services to link (the overlays' `$(LLAMA_BIN)` prerequisite was a run-time
need; the four scripts now `make <tool> llama` where they run the model), so `ci.yml`'s hosted job builds
`make tools` after `make all`. All six compiled first time, with no warnings. A seventh overlay remains in
`tools/answer_unity_query_probe.py` (and per-mutant overlays in `test_recall_mutations.py`); the audit's six did not
name them and they were left.

`make mutationtest` runs the five controls that need no services and `make mutationtest-db` the three that do,
each with `--output build/mutationtest/<name>` (the five without `--output` gained it, defaults unchanged): their
default directories (`build/audit-mutations`, `build/memory-mutations`, ...) are cited and archived, and a gate must
not write into evidence. Trusted acceptance runs both after the services start (`acceptance.yml`, about six minutes
in all here).

Running them found four controls rotted since the code they mutate moved, exactly the F9 risk:
- `history_worker`: the store mutants no longer linked (`_file_text_write_atomic`; `test_store_connections` has
  linked `src/core/file_text.c` since `de82f6d`). Added.
- `memory_pipeline`: the planner mutants' hand-kept source list lacked `recall_terms.c` and `experiments.c`. It is
  now read from the Makefile's `CONTEXT_PLANNER_TEST_SRCS`.
- `product_memory`: `copied_tail_excluded` no longer matched (the loop is now `b->first_history`/`b->history`).
- `recall`: `lexical_length_norm` no longer matched (the column became a `%s` argument when the index-text
  experiment was retired).

Each was re-pointed at the same guard and is killed again. Results: `mutationtest` exit 0 in 2 min 29 s (every
control passed, every mutant killed; `item3-mutationtest.log`), `mutationtest-db` exit 0 in 6 min 2 s against the
local native services, throwaway databases (`item3-mutationtest-db.log`). Gates on the committed tree (item 4's
work set aside with a uniquely tagged stash, applied back by its SHA and dropped by that SHA): `make test` exit 0
after the amend described next, `make analyze` exit 0, `make tools` exit 0 (`item3-*.log`). The first `make test`
failed in `evidence_check`: TESTING.md now names `build/import_probe`, `build/chat_hang_probe` and
`build/mutationtest/<name>`; they were added to the not-evidence list and the commit amended.

## 4. F4, scripts: every arm pinned and recorded (`83b0ca2`)

`tools/experiment_arms.py` reads the flag table from `src/core/experiments.c` (so a new flag is pinned without
editing it) and pins every flag the caller left unset to its default stated as a value: `flag_on` flags `1`,
`flag_is(..., "1")` flags `0`, other `flag_is` flags `off`; `SCRIBODO_SEM_DEBUG`, which any value turns on, stays unset
and is recorded as unset. Today's line: `ARMS SCRIBODO_ANSWER_PARTS=0 SCRIBODO_ANSWER_BLOCK=off SCRIBODO_MEMORY_TRACE=0
SCRIBODO_MEMORY_EXCERPTS=0 SCRIBODO_MEMORY_SMALL_UNITS=0 SCRIBODO_SEM_DEBUG=unset SCRIBODO_MEMORY_EXPERIMENT=1
SCRIBODO_MEMORY_BRIEFS=0 SCRIBODO_ANSWER_PIPELINE=1 SCRIBODO_MEMORY_PRESENTATION=off SCRIBODO_OBSERVER_CONFLICT_GATE=0
SCRIBODO_OBSERVER_SENTENCES=0`.

Decision: the default pinned is the product's current default (`SCRIBODO_ANSWER_PIPELINE=1`), not the pre-flip
value. A baseline is then the product as it ships, and the arm is in the run; `SCRIBODO_ANSWER_PIPELINE=off`
reproduces a run from before 2026-11-14 (stated in both scripts' usage). With the fake model the pipeline still runs
only for `verified` (`main.c:3857`), which the recorded value shows.

Sweep: `manuscript_baseline.sh`, `manuscript_restart.sh` (plus `arms.json` in its `--out`), `history_frame_soak.sh`,
`memory_pipeline_soak.sh` and `frame_soak_run.py` (the launcher under the manuscript and soak scripts and
`answer_product_run.py`) pin and print the line; the memory reach, quality (with its `SCRIBODO_MEMORY_BRIEFS=1`),
native and operation evaluations, `answer_product_run.py` and `answer_gate_replay.py` pin the driver's environment
and record `experiment_arms` in their manifest or output. `manuscript_series.sh` runs `manuscript_baseline.sh`; the
`*_arm.py` tools drive llama-server from Python and set no product flag. `tests/test_experiment_arms.py` checks the
parse against `experiments.h`'s struct and fails when any tool other than `test_*`/`observer_*` sets a flag in code
without the pin. `tools/observer_replay_run.sh` (sets neither observer flag) is the observer stream's.

Verified live: `memory_pipeline_soak.sh --seconds 30` printed the ARMS line and passed (`PIPELINE_RESULT result=PASS`,
database cleaned, exit 0; `item4-pipeline-soak.log`); `SCRIBODO_ANSWER_PIPELINE=off memory_reach_eval.py
--prepare-only` wrote `"SCRIBODO_ANSWER_PIPELINE": "off"` with the other ten pinned into its frozen manifest (the
prepared run was removed). The manuscript scripts need the display and were checked with `bash -n` and the unit
test only. Gates: `make test` exit 0, `make analyze` exit 0 (`item4-*.log`).

## 5. F12, tooling (`2d84a12`)

`tools/memorilla_map_probe.py` is `tools/archive/memorilla_map_probe.py` (its `ROOT` one directory deeper, its usage
line and the journal's reference updated), with `tools/archive/README.md`: nothing deleted, only archived, runnable,
moved back if the experiment reopens.

`tools/answer_common.py` holds `ROOT`, `sha`, the GGUF pin and the Unix-socket client (`UnixHTTP`, `call`,
`private_socket`). Twenty-one answer tools imported `ROOT`, `sha` or the GGUF pin from `memory_causal_parity` (a memory
probe that loads transformers' tokenizer) and the socket helpers from `answer_bedrock_probe`; 22 now import them from
`answer_common`, and `answer_bedrock_probe` re-exports them (the same objects; tested). Imports of
`memory_causal_parity` by answer tools fall from 21 to 7, which keep only its `tokenizer`, `SNAPSHOT` and `CODE`.
`memory_causal_parity` keeps its own copies. Not reduced: the `sys.path.insert` lines (14 to 15, the probe gaining
one). The tests load tools by file path, so a tool that imports a sibling must put its own directory on the path;
moving that into the tests would add a line to each of 28 tests instead. Tools with only a local `ROOT` line and no
sibling import keep it: importing one constant would cost them that bootstrap line. `tests/test_answer_common.py`
is in `make test`; every `tests/test_*.py` passes (one test's module stubs gained `answer_common`).

### The tsan count (AUDIT-003 48,488, AUDIT-004 48,352)

Extracted `build/audit003/tsan.log` from `scribodo-evidence-2026-11-11-audit003.tar.gz` into a scratch folder of the worktree and compared it with `build/audit004/tsan.log`. Both logs build and run the same 18
binaries in the same order; twelve print an "N checks, 0 failures" line. Per binary, AUDIT-003 to AUDIT-004:
`test_memory_document_cache` 34,021 = 34,021, `test_memory_budget` 20 = 20, `test_generative_gate` 174 = 174,
`test_http_deadlines` 117 = 117, `test_db_deadlines` 146 = 146, `test_memory_brief_model` 154 = 154, `test_llm`
4,690 to 4,741 (+51: new cases `build_base_request`, `observer_judge_log`, `observer_sentence_pairs`,
`observer_wait_mode`, `orphan_after_owner_kill`, `reap_orphan`), `test_components` 104 to 105 (+1),
`test_remote` 149 = 149, `test_answer_pipeline` 188 to 296 (+108), `test_chat_view` 382 = 382,
`test_semantic_faults` 8,343 = 8,343. The real AUDIT-004 total is **48,648**, 160 above AUDIT-003, every count at
or above it. 48,352 is 48,648 minus 296: AUDIT-004's sum missed `test_answer_pipeline`'s line, the only one with a
prefix (`answer_pipeline: 296 checks, 0 failures (...)`), while AUDIT-003's 48,488 included its 188. No binary lost
checks; the difference was the summing pattern.

Gates: `make test` exit 0, `make analyze` exit 0, `make tools` exit 0 (`item5-*.log`).

## Red-team remediation (2026-11-15, same branch)

The red team found five faults in the work above; each is one commit.

1. **A cited directory passed when one file under it was archived** (the audit's own mistake, repeated):
   `build/gap003` passed with 32 files under it unarchived, `build/hangs` with one. A cited path that exists now
   passes only when every file it denotes is archived or declared not evidence; relative names in backticks
   (`gates-exit.txt`, `series-real-3/*`) resolve under a directory the same document cites; files are looked for in
   the main checkout's `build/` (git's common directory), also from a worktree, and the summary names the folder.
   The 33 files are `scribodo-evidence-2026-11-15-audit004c.tar.gz` (SHA-256
   `b3c358881bd7f3dd905348dfbb4d70fff7cc3cff4c977ff2e3fb61dbaf463939`).
2. **A not-evidence entry excused everything under it** (a crafted `fit-on3/result.json` under `build/ui` passed).
   Entries now match one path; `*` stays within a component; only `DIR/**` covers a subtree; the lost list likewise.
3. **Seven lost entries were archived file stems** (`build/relation/rel4-before`, `build/recover/gates2`,
   `build/gap003/{gates,frames/gates,orphan/gates,reap/gates,soak-smoke}`): a stem resolves to `stem.*` and
   `stem-*` members. Eleven entries remain: `build/ui/fit-on`, `build/ui/fit-native2`, `build/cr1/score.json`, the two
   `build/mb-*/events.jsonl`, `build/memory-causal-parity.log` and `-v2.log`, `build/memory-continuity-*`,
   `build/memory-safety-*`, `build/redteam2-*.log`, `build/remediation-*.log`. Item 1's statement that seven AUDIT-004
   paths were lost is therefore wrong for those seven: only `fit-on` and `fit-native2` were.
4. **`experiment_arms` would have dropped the observer flags** once remed/observer merges (read in level helpers
   outside `read_environment`) and ignored `SCRIBODO_FAKE_EXTRACT_MS`. Every `SCRIBODO_*` literal is now classified
   (arm, level, switch, test hook) or the run stops; tested on remed/observer's `experiments.c`.
5. A missing archive folder now fails unless `CI` or `SCRIBODO_EVIDENCE_SKIP` is set; `build/servicestest-logs` is
   gate-owned; the answer tools that record hashes record `answer_common.py`'s.

Gates per commit (`build/audit004-tooling/r*-*.log`, archived in `scribodo-evidence-2026-11-15-audit004-tooling-r`): `make test` exit 0 and `make tools` exit 0 after each; `make
analyze` exit 0 after the first; after commits 2 to 4 it was skipped because the busy check (`pgrep -f "clang
--analyze"`) matched its own shell's command line, not another analyzer, and it ran once on the final tree with a
check on the process name instead, exit 0 (the later commits change no C). `make mutationtest` was not rerun: nothing in this round touches it.

## Verification holes (2026-11-15, same branch)

The coordinator's verification closed the five findings and found three more holes and four small items.

- A: a placeholder not-evidence entry (`build/ui/<script>`) excused concrete paths and skipped their file walk, so a
  recreated `fit-on` passed as lost. Placeholder entries now match only placeholder citations, only an exact entry
  skips the walk, and a lost entry with any file on disk fails.
- B: a watchdog dump written before `make test` failed every citation of the directory `build/hangs`. Entries can be
  marked `walk` (they excuse only files found under a cited directory); `build/hangs/*-*.txt` is one, so a dump cited
  by name is still checked.
- C: `experiment_arms` took a one-line `getenv` before a level helper for a level. A level now needs
  `v = getenv(X)` and `off_value(v)` in the same brace-matched function.
- Small: Markdown links and angle brackets end a citation; a spaced path is cited only inside backticks; a missing
  archive folder skips only on a GitHub-hosted runner or with `SCRIBODO_EVIDENCE_SKIP`; two more answer tools hash
  `answer_common.py`; a bare `build/` is reported as the folder.

Gates (`build/audit004-tooling/rA-*.log` to `rD-*.log`, archived in `scribodo-evidence-2026-11-15-audit004-tooling-v`): after
each commit `make test` exit 0, `make tools` exit 0 and, with no analyzer process running (checked by process name),
`make analyze` exit 0.

## Not done here

- `git gc`, README, `docs/EVIDENCE.md`, `docs/TECHNICAL-DEBT.md` and the audit document belong to the coordinator;
  the EVIDENCE.md paragraphs for the two new archives are in the stream's report.
- The observer yardstick's expected counts (717 / 348 / 948, F9's second half) and `observer_replay_run.sh`'s arms
  are the observer stream's.
- uitest, uitest-db, backuptest, llmtest, uitest-real and offlinetest were not run (the display and the model are
  shared); the uitest recipe change was rehearsed with a stub app as described in item 2.

## Evidence

The gate logs above (`build/audit004-tooling/item*-*.log`: every item's test, analyze and tools logs, the mutation and soak logs) are in
`scribodo-evidence-2026-11-15-audit004-tooling.tar.gz` with its manifest and `SHA256SUMS` entry; the extracted
AUDIT-003 log is not re-archived (it is in its own archive).
