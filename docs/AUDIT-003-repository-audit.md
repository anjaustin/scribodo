# AUDIT-003: technical-debt audit after the verified-pipeline cycle

**Date:** 2026-11-11
**Baseline:** `bd23f8b` (`Quotation integrity check; treatment rerun shows zero wrong statements shown`)
**Previous audit:** [AUDIT-002](AUDIT-002-repository-audit.md) at `6c243d5`; 20 commits
have landed since.
**Scope:** the whole tree, end to end: every verification target including the
Docker-backed and real-model paths, four read-only reviews of the source by area
(`src/app`, `src/llm`, `src/main.c` with `src/ui` and `src/core`, and the
tooling, tests, docs and CI), the debt register's standing claims re-checked
against the current tree, and repository hygiene. This audit looks for debt,
meaning what makes the next change expensive or the next claim unreliable,
rather than individual defects; defects found on the way are recorded.

Logs are kept under the untracked `build/audit003/` directory and archived
(see [EVIDENCE](EVIDENCE.md)).

## What landed since AUDIT-002

| Area | Files | Lines added | Lines removed |
|---|---:|---:|---:|
| `src/` | 18 | 2,141 | 39 |
| `tools/` and `tests/` | 46 | 5,503 | 15 |
| `docs/` and `journal/` | 21 | 3,566 | 2 |
| whole tree | 88 | 11,246 | 58 |

One line in five is product code. Half is experiment tooling and its tests;
a third is prose. Almost nothing was removed. That shape is the audit's
first finding: the cycle method produces evidence and instruments faster than
it retires them, and the register has no entry for either.

## Verification matrix (unchanged tree)

All C and Objective-C sources were touched so that the `all` target recompiled
them with warnings visible. Targets ran sequentially; wall times are in
`build/audit003/results.tsv`, per-target output in `build/audit003/<target>.log`.

Sixteen of seventeen targets exited 0 in **843.2 s** of sequential wall time.

| Target | Result |
|---|---|
| `all` | 89 compile commands after touching every source, **0 compiler warnings** |
| `analyzetest` | 8 Python gate tests passed |
| `test` | **95,813 C checks** in 35 binaries and **142 Python tests** in 25 files, 0 failures |
| `analyze` | 0 warnings across 86 C/Objective-C files |
| `dbtest` | **FAILED**: 3 of 11,339 checks, all in `test_db_core` migration (F1); the other nine binaries passed |
| `sanitize` | same 95,813 checks and 142 Python tests, 0 failures; 0 ASan/UBSan diagnostics |
| `tsan` | **48,488 checks** in 12 binaries, 0 failures; 0 ThreadSanitizer diagnostics |
| `uitest` | 398 passed, 0 failed (23 scripts) |
| `uitest-db` | 281 passed, 0 failed (15 scripts) |
| `backuptest` | 38 UI assertions, 0 failed |
| `remotetest` | 7 passed, 0 failed |
| `cstest` | 3 passed, 0 failed |
| `dependency-report` | every pin and container digest verified; no OSV advisories |
| `llmtest` | 24 real-model checks, 0 failures |
| `uitest-real` | 35 passed, 0 failed (3 scripts) |
| `offlinetest` | 110 script passes plus 24 real-model checks, 0 failed |
| `tools/test_audit_mutations.py` | 10 mutants killed, 0 survived; both controls passed |

**Remediated tree** (migration 015 with `IF NOT EXISTS`): `make dbtest` passes,
**18,000 checks** in 10 binaries, 0 failures (`build/audit003/dbtest-after.log`).
No other target was re-run; the change is one SQL string.

Other checks: `git fsck` clean; 0 tracked-but-ignored files; all seven
submodules at their pins; 0 `TODO`/`FIXME`/`HACK` markers in `src/` (the 32
hits for `XXX` are all `mkstemp` templates); 0 `#if 0`.

## Findings

### F1 — migration 015 is not idempotent; `make dbtest` fails at HEAD (P2, fixed)

`k_migration_015` (`src/app/db.c:862-866`) adds `memory_units.index_text` and
its full-text key with a plain `ADD COLUMN` / `ADD FULLTEXT KEY`. Every other
`ALTER` in the migration list says `IF NOT EXISTS` (24 statements, `db.c:638-793`),
because the migration test deliberately deletes receipts and re-applies the
migrations to model a crash between DDL and receipt. Against the current tree
that replay fails:

```
FAIL tests/test_db_core.c:161: db_migrate(db, err, sizeof(err)) && db_schema_version(db) == 15
FAIL tests/test_db_core.c:163: ...
FAIL tests/test_db_core.c:165: ...
63 checks, 3 failures
```

The other nine database binaries pass (11,276 checks). The commit that added
the migration (`2e05ca1`) and the journal entry for it
(`journal/answer-cycle3_coref.md:206`) both record `make dbtest` as passing.
That claim does not hold against the tree and was not re-run for the two
commits after it. This is the audit's fidelity finding as much as its defect
finding: a gate result recorded in prose was not the gate's exit status.

**Remediation.** Both clauses now carry `IF NOT EXISTS`; `make dbtest` passes
(see the remediated row below). The fix is in the same form as migrations
010 to 013.

### F2 — four register claims no longer hold (P2, register corrected)

| Register claim | Status in the tree |
|---|---|
| *Weakly typed DB rows*: "Production code has no direct indexed-cell reads" | `src/app/memory_gc.c:254-256` reads `db_cell(&r, 0, 4/5/6)` straight into `snprintf`, with no `db_rows_has_columns` check. Added 2026-11-07, after the row was written; present at AUDIT-002's baseline and not caught there. Not a defect today (the columns are `NOT NULL`), but the convention has no mechanical guard: nothing in the Makefile or CI greps for `db_cell(`. |
| *Storage policy leaks*: "`store_mode()` is private to store resolution and transition orchestration" | Eleven call sites. `memory_job.c:427,451` and `memory_documents.c:90,206` are ordinary memory-domain workflows written after the entry closed; `store_backends.h:3` still says modules "not yet migrated may still resolve store_mode() directly". |
| *Hidden process-global state*: Complete | `src/app/memory_index_text.c:21-27` is a new process-global table (`static` struct, `PTHREAD_MUTEX_INITIALIZER`, `pthread_once_t`) initialised from `getenv` on first use and calling `abort()` on load failure. It is the only file-scope mutable static under `src/app/` besides the documented thread-local in `store.c:55`, and it is not in the register's list of intentional globals. |
| *`main.c` coordination bottleneck*: Complete, including "Separate a read-only frame view model from layout construction" | `FrameViewModel` is a four-field string arena. `build_layout` (`src/main.c:2956-3202`) makes 103 direct reads of `App` fields across about 25 fields and none through the view model. The checkbox solved string lifetime (acceptance criterion 3), not the separation it names. Criterion 2 ("composition root and event loop rather than the implementation of every workflow") is not met: `finish_prompt` (178 lines), `send_chat` (121), `open_source` (77) and five more workflow bodies remain in the file. `main.c` is 4,555 lines, up 763 (20%) since the entry was marked Complete on 2026-11-05 and 22 since AUDIT-002; the P2 row's 5,000-line trigger is 445 lines away at a recent rate of about 127 lines a day. |

The register entries are amended below rather than reopened wholesale: each
is a specific regression or overstatement against an otherwise sound seam.

### F3 — experiment flags are a second configuration system with no owner (P1, new register entry)

Thirty-five `SCRIBODO_*` variables are read in `src/`. Nine of them are
memory or answer experiments, read in six files with ad hoc parsing at each
site: `main.c` (four, pushed into `ChatSession` through five setters),
`chat.c` (`SCRIBODO_ANSWER_BLOCK`, `SCRIBODO_SEED` twice, `SCRIBODO_MEMORY_TRACE`
three times), `answer_pipeline.c` (`SCRIBODO_ANSWER_PARTS`),
`context_planner.c` (`SCRIBODO_MEMORY_EXCERPTS`, `SCRIBODO_MEMORY_SMALL_UNITS`),
`memory_document_store.c:244` (`SCRIBODO_MEMORY_SMALL_UNITS` again, read per
ingest step inside a store function) and `memory_index_text.c`. Two of the
answer flags are "measured, default chosen" switches whose comments say the
experiment is over (`chat.c:797-799`, `answer_pipeline.c:934-936`), still
compiled into the product. `SCRIBODO_ANSWER_BLOCK` appears in no document.

The flags compose. `build_in_worker` carries five independent mode axes
(pipeline, three-valued presentation, briefs, canon reference, remote or edit)
for at least 24 reachable combinations, of which the live runs exercise two.
Schema migration 015 exists only to serve `SCRIBODO_INDEX_TEXT_FILE`; its
column is carried by every unit hydrator (`memory_store.c:398-401`,
`memory_document_store.c:350-353`), by `semantic.c:1169-1188` and by
`answer_pipeline.c:977-994`, and `memory_index_text_match_column()` swaps the
SQL `MATCH` column at run time by environment.

None of this is wrong for an experiment. The debt is that no entry says which
flags are experiments, what decides each one's fate, or when a schema column
added for one is removed or made permanent. The project's own rule (fix
software, measure prompt changes) makes this the area most likely to keep
accreting flags, so it needs the lifecycle written down.

### F4 — the receipt and trace protocol is an untested cross-language contract (P2, new register entry)

The pipeline writes receipts into the stored reply (`--- VERIFIED SOURCE ... ---`
at `answer_pipeline.c:1097-1103`; `--- ANSWER GATE | ... ---` and
`--- ANSWER QUOTES | ... ---` at `chat.c:1492-1494`) and trace lines to stderr
(`ANSWER_PIPELINE`, `ANSWER_PIPELINE_SCORES`, `ANSWER_GATE`, `MEMORY_PREPARE`).
`tools/answer_pipeline_review.py:20-30` parses them with six regular
expressions. No document describes the markers and no test covers the
round trip; `ANSWER GATE` and `ANSWER QUOTES` occur in zero docs and zero
tests. The receipts persist in `messages.memory_sources`, which the store
bounds at 16 KiB (`store_chats.c:110`, `chat_store.c:83`); nothing asserts
that a receipt stays under it. A marker change is therefore both a tool break
and a stored-data format change.

### F5 — three worker functions are monoliths again (P2, new register entry)

`build_in_worker` (`chat.c:1082-1389`) is 308 lines with about 176 decision
points (49 `if`, 72 `&&`, 10 `||`, 33 `?:`, 12 `for`). It owns history copy,
token budgeting, store binding, the pipeline branch, gate arming, receipt
install, two trace emitters, plan, briefs, episode and canon admission,
sampling, remote adaptation and notice composition. `chat_answer_pipeline_configure`
has no test caller and `answer_pipeline_run` has no C test; the pipeline's
pure functions are well covered but its orchestration is covered only by live
runs. `run_job` in `observer.c:534-818` (285 lines) and `observer_job_find`
(`observer_job.c:184-417`, 234 lines) have the same shape; the P2 row
"Observer monolith" is marked Done. In `src/app`, `script.c` `run_line` (288)
and `run_expect` (198) are a test interpreter compiled into the product
binary (`Makefile:78`), with 17 `host_*` adapters and 24 script-conditional
sites in `main.c` and no `#if` anywhere; the register lists the `input.c`
globals as intentional but not the compiled-in interpreter.

### F6 — evidence and environments behind the cycle's claims are unarchived and undeclared (P2, partly remediated)

- `docs/EVIDENCE.md`'s latest archive is `2026-11-09-cycle`. Since then the
  journals cite about 60 new `build/` entries (`cr1`, `cr2`,
  `answer-live-coref{,2,3}`, `answer-live-pipeline{,2..7}`,
  `answer-pipe-capture`, `gate-replay-*.json`, `redteam021-mutations`), none
  archived. The accepted pass-6 design and the coreference decision rest on
  them. `TESTING.md:619-621` promises archival before deletion; the promise
  holds only if someone archives before the next `make distclean`. Archived
  by this audit; see below.
- The experiment Python has no declared environment: no `requirements*.txt`,
  `pyproject.toml` or lock anywhere. Two virtualenvs live under the ignored
  `build/` (`venv-coref` 1.1 GB, `venv-causal` 797 MB); versions exist only in
  journal prose and run manifests. `tools/` imports torch in 7 files,
  transformers in 7, huggingface_hub in 5, spaCy and fastcoref in 1 each.
  `make distclean` deletes both environments.
- `tools/answer_gate_replay.c` is built by a Makefile rule but no phony target
  depends on it; the `gate-replay-*.json` evidence depends on a hand-invoked
  build.

### F7 — verification outcomes were not carried back into the record (P3, recorded)

- Hosted run `42927765274` on `e3d3324` failed its analyzer job (a garbage
  read in `compound_number` and a null dereference in `answer_gate`); `f648127`
  fixed it and the only record is that commit's message. AUDIT-002 F4 made the
  same finding about the previous cycle.
- Four commits (`8496ac8`, `2e05ca1`, `71d9302`, `bd23f8b`) are unpushed;
  `origin/main` is `5fcf979`, green. No hosted verdict exists for the index
  layer, the gate change or the quotation check.
- The register says "Last reviewed: 2026-11-09"; an entry was added on
  2026-11-10 and 20 commits landed without a review line.
- The local branch `fix/persistence-audit-redteam` is patch-equivalent to the
  merged `938e51b` (PR #2) and can be deleted.

### F8 — duplication that raises the cost of the next change (P2 and P3, new register rows)

- **Atomic file writes** are hand-rolled seven times (`observer_store.c:164`
  and `:468`, `recent.c:203`, `store_import.c:658`, `store.c:419`,
  `revisions.c:53`, `core/doc_io.c:304`), with parent-directory `fsync` in
  three of them and an `EINTR` loop in one. `profile.c:61-80` and
  `model_settings.c:119-138` use a fixed `.tmp` name with no uniqueness.
- **Unit size** has a name (`MEMORY_UNIT_BYTES 2000u`, `memory_store.h:10`)
  and about 15 literal copies (`2000`, `2003`, `2004`) across
  `memory_store.c`, `memory_document_store.c`, `recall.c`, `db.c:755-757`,
  `semantic.c:744,777`, `rerank.c:16`, `memory_brief.c:323`; the window
  arithmetic is duplicated verbatim between the chat and document ingesters.
  `slot_ctx - MAX_REPLY_TOKENS - 256` appears five times in `chat.c`.
- **Chat-completion request bodies** are assembled by hand in five files with
  per-site timeout literals (15, 30, 60, 120 and 180 seconds); `http.c:332`
  substitutes 30 seconds for a non-positive timeout silently. The HTTP client
  still opens one connection per request (14 call sites, five of them new
  since AUDIT-002); a verified-pipeline turn costs reranker, extraction,
  reply and gate connections.
- **Headers**: `store.h` (28 exports) is included by 37 files; `chat.h`
  (50 exports, 10 "for tests") by 16 files across `ui/`, `app/` and `main.c`,
  and `memory_store.h:8` includes `llm/chat.h` for one enum, so `app` and
  `llm` include each other.
- **Tooling**: 35 `tools/answer_*.py` scripts (5,290 lines) share copied
  scaffolding (`ROOT = Path(__file__).resolve().parents[1]` 43 times; 13
  `sys.path.insert` sibling imports; four AST-identical helper bodies); 11 have
  no test and 7 are referenced by one journal or one doc only.
- **Makefile**: 44 per-binary link rules, 18 hand-written TSan compile lines
  and a 25-line Python test list; zero drift today, two or three edits per new
  test.
- **UI**: all Clay layout lives in `main.c` (31 `CLAY(` sites, none in
  `src/ui`); adding the recall view cost `main.c` 150 lines. `App` has 143
  fields with four anonymous unions aliasing the extracted controllers back
  onto flat names. Escape is handled at seven sites with its own modal guard
  each; the arm-then-confirm idiom is copied three times. 76 colour literals
  across ten files and no theme header.
- **Dead or test-only exports**: `chat_set_canon_reference` has no caller, so
  `canon_reference.c` (119 lines) and the canon block at `chat.c:1322-1335`
  are unreachable in the product; `revisions_list_files`, `import_job_running_for`
  and `renames_resolve` have no callers at all; `import_job.c` has no test.

## Checked and in good shape

- Test inventory has zero drift: 26 Python tests on disk equal 25 in
  `make test` plus `test_analyze`; all 43 C test sources have Makefile rules;
  every built binary runs in `test`, `dbtest` or `tsan`.
- 120 tracked Markdown files, 204 relative links, 0 broken; `git fsck` clean;
  all seven `third_party/` entries are pinned submodules; container images
  and GitHub Actions pinned by digest or SHA; the reranker weights pinned by
  SHA-256; 0 `TODO`/`FIXME` markers in `src/`; 0 `#if 0`.
- Return conventions are uniform in `src/app` (207 `bool` exports, one
  error-buffer shape) and the three-way file/database/paused dispatch is
  genuinely in one ops table.
- The pipeline's pure functions (`answer_pipeline.c`) have ten test groups,
  TSan coverage and six of six mutations killed.
- Background workers in `main.c` share one lifecycle (create, poll, destroy),
  the frame thread never joins or sleeps, and there is one static global in
  the file.
- Every pipeline decision in the last two cycles is traceable to a dated
  journal measurement; the measure-before-ship rule is followed.

## Register changes made by this audit

See the 2026-11-11 session log in [TECHNICAL-DEBT](TECHNICAL-DEBT.md): the
four claims in F2 are amended in place; F3, F4, F5, F6 and the F8 rows are
new entries; F1 is recorded as fixed; the `main.c` size row is re-measured.

## Evidence

The matrix logs, `results.tsv`, the failing and remediated `dbtest` logs, and
every `build/` directory the cycle-3 journals cite are in
`scribodo-evidence-2026-11-11-audit003.tar.gz` (774 files, instrumented
binaries excluded); see [EVIDENCE](EVIDENCE.md) for the manifest and SHA-256.
The four subsystem reviews' scratch scripts (function-length and dead-export
scans, link checker, Python import scan) were session files and are not kept;
every figure they produced is re-derivable from the tree with `grep`, `wc` and
`git log`.
