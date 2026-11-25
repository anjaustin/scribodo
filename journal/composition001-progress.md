# COMPOSITION-001 progress and handoff

Canonical work ledger: [COMPOSITION-001](../docs/COMPOSITION-001-plan.md).

## Current handoff

- **Active P0:** lightweight composition, authorized 2026-11-25.
- **Completed slice:** C01 and REDTEAM-025 — file chat failure guards / retry acknowledgement (AUDIT-005 F1/F3),
  with passing unit/analyzer/sanitizer/race, file UI, isolated DB/crash/DB UI gates.
- **Active slice:** C02. Consumer map, identity/compatibility constraints and red synthetic contract probe
  are recorded in `docs/COMPOSITION-001-revision-identity.md`. F2 is not fixed yet.
- **Next action:** extend the isolated C02 resolver with logical history enumeration and archive/rename
  generations, old-source token lookup and exact path-bound/symlink behavior. Reversible keys currently
  have the smaller ownership-publication protocol, but are not selected for production until these tests
  and the legacy eligibility rule are settled. Do not migrate real projects.
- **Immediate author instruction:** REDTEAM-026 verification passed; commit and push the prototype/design
  checkpoint, then continue F2. No production format change is part of that checkpoint.
- **Baseline:** `69b98abf1ecf86c560b435a209327175c182eab1`.
- No production migrations or dependency pin changes. Author explicitly authorized the baseline and
  remediation commits on 2026-11-25; baseline committed as `b0f81a3`.
- Prior uncommitted docs/architecture/audit/research artifacts are session work to retain.

## 2026-11-25 — plan decisions

### Scope and options

The author promoted the component-composition research to P0 and delegated routine execution. The research
identified several independent opportunities, not an approved bulk replacement: embedded SQLite/FTS5 can
remove database operations; embedded exact vectors may remove Qdrant; native AppKit text can replace custom
input/layout duties; utf8proc can replace narrow Unicode logic; existing libcurl can consolidate transport.
Each alternative still needs comparative measurements, license review and the same safety contracts.

The baseline already reuses mature libraries. Its custom authority/provenance and source-admission code is
not disposable plumbing. Model allocations may dominate service/index savings. An embedded store changes
writer concurrency and migration/recovery behavior. A native editor changes index units, composition and
undo ownership. Bringing in all candidate libraries at once would make attribution and rollback impossible.
Keeping every backend indefinitely could increase precisely the code burden this work aims to reduce.

Open decisions before promotion: Which exact-search scale exceeds the agreed latency budget? Does a native
text island preserve source anchors and real input while removing more code than its bridge adds? Can
SQLite's writer contention and backup/recovery meet the whole-workload contract? What is the actual net
process/model memory difference? Which packaging/model-rights obligations remain? These are experiment
questions, not reasons to delay the first deterministic safety repair.

### Constraints and dependencies

1. Original bytes/identities and explicit author authority are invariants.
2. F1/F3 are caller persistence-state defects, independent of future backend choice.
3. F2 needs a compatibility design because old directory collisions can already be ambiguous.
4. Runtime advisories need reviewed fixes/precision gates, not blind latest-version adoption.
5. A controlled baseline precedes cost claims; a safety regression blocks promotion.
6. Storage/retrieval can be tested separately; Qdrant has two distinct indexing protocols.
7. Native editing is independent of SQL selection but must share one edit/undo authority.
8. Added neural extraction is last, after deterministic literary/Unicode improvements.

### Decision

**Remove responsibilities only after a replacement proves cheaper end to end and preserves the author contract.**
Start with the small caller repair rather than bundling it into a database migration. Keep current
services as controls while prototypes stay disposable. Put session state in one canonical ledger and this
handoff, with entry-point pointers instead of several competing plans.

### Adversarial checks and refined state model

- “Fewer daemons is necessarily lighter” fails if recovery/adapter code or model allocations dominate.
  Require net system measurements and permit rejection.
- “A file retry is a DB save without a sequence” fails if it activates a DB window or fabricates sequence 0.
  Model acknowledgement and sequence assignment independently.
- “All unknown sequences are unsaved” fails for normally loaded file chats.
  A file-backed guard checks actual failed appends, while a DB window additionally checks missing sequences.
- “Clear failure on any retry” fails on refused/uncertain writes. Clear only acknowledged success;
  retain failure state otherwise. Do not add an automatic retry loop in this slice.

| Event | Failure mark | DB sequence/window |
|---|---|---|
| Normal loaded file message | clear | unknown / inactive |
| Refused append | set | unchanged |
| Acknowledged file append | clear | unchanged; no invented sequence |
| Acknowledged DB append | clear | assigned / active |
| Retry fails | retained | unchanged |
| Retry runs after acknowledged success | no append | unchanged |

Implementation/test results follow below; the planning checks are design outcomes, not runtime evidence.

### C01 implementation and initial evidence

- Added headless regressions for file-backed failure visibility, refusal to replace/send through an unsaved
  conversation, invalid acknowledgements, DB-window distinction and repeated retry duplication.
- `chat_window_info` now exposes actual failed appends in either backend. `chat_mark_saved(..., -1)` clears
  an acknowledged file append's failure without inventing DB identity; DB acknowledgements retain their
  existing window/sequence behavior. Callback, adoption and explicit retry paths acknowledge both backends.
  Failed-save messages cannot be trimmed merely because they carry an older known sequence.
- `make build/test_llm > journal/composition001/c01/red-build.log 2>&1 && tools/watchdog.sh 300 composition-c01-red ./build/test_llm > journal/composition001/c01/red-test.log 2>&1`
  exited **1** against the original implementation; the build succeeded and regression assertions failed.
- `make build/test_llm > journal/composition001/c01/green-build.log 2>&1 && tools/watchdog.sh 300 composition-c01-green ./build/test_llm > journal/composition001/c01/green-test.log 2>&1`
  exited **0** after the fix: **5,111 checks, zero failures**.
- `git diff --check` exited **0**.
- Original C01 gate outcomes:
  - `SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make test > journal/composition001/c01/test.log 2>&1`
    — completed **0**. Optional observer-branch check skipped because that branch is absent.
  - `make analyze > journal/composition001/c01/analyze.log 2>&1` — completed **0**:
    **zero warnings across 113 source files**.
  - `SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make sanitize > journal/composition001/c01/sanitize.log 2>&1`
    — first attempt **2**, documentation gate only; corrected rerun **0** (see below).
  - `make tsan > journal/composition001/c01/tsan.log 2>&1` — completed **0**; all reported
    checks passed, including **5,111** in `test_llm`; no ThreadSanitizer warning reported.
- The environment permits missing evidence archives and untracked-novel yardsticks; on this host the
  retained logs show both checks actually ran (953 archived citations; yardsticks 717/348/948 lines,
  byte-identical). Earlier wording calling these checks excluded was incorrect and is corrected here.
  Real GUI disk-full,
  VoiceOver/IME, database/crash/UI-db and real-model gates have not run. Mechanical success is not UI or
  model-quality acceptance. No services or real author stores were migrated; no commits were made.

### Gate correction

The first `make sanitize` invocation exited **2** at `evidencecheck`, before executing the C sanitizer suites:
the journal's exact commands cite the `test_llm` executable, which was not classified as a non-evidence build
artifact. Added that specific binary to `tests/fixtures/evidence_not_archived.txt`, consistent with existing
test/tool-binary entries. This does not exempt any result or log. Preserve the original `sanitize.log`;
the corrected rerun uses `sanitize-rerun.log`.

`SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make sanitize > journal/composition001/c01/sanitize-rerun.log 2>&1`
completed **0**. ASan/UBSan suites passed; archive/yardstick checks ran on the present inputs, with the optional
observer-branch skip. No sanitizer finding was reported.

`make all > journal/composition001/c01/build-app.log 2>&1` completed **0**, verifying the production
callback/adoption translation unit and app link in addition to the headless suites.

### C01 disposition

Mechanically verified: focused red/green, full `make test`, `make analyze`, `make sanitize`, `make tsan`
and `make all`. Changes remain uncommitted. This resolves the demonstrated state/retry behavior in the
headless tests, not exactly-once persistence after an ambiguous write. Real UI failure handling and
isolated database/crash/UI-db suites remain unrun. Do not infer acceptance of those workflows from this status.

## 2026-11-25 — REDTEAM-025

Author instruction: commit the current work, red-team it, remediate/verify iteratively, then commit the
remediations. `git commit -m "Establish composition P0 and repair file chat save acknowledgements"` exited
**0**, creating **`b0f81a3`**. This is the review checkpoint, including preserved earlier audit/research notes.

Findings and scope: [REDTEAM-025](../docs/REDTEAM-025-composition-save-state.md), R1–R5.
The repair now shares owned-message snapshots and ordered acknowledgement across callbacks, adoption and
retry. The app marks a failed callback preparation; adoption protects its whole obligation before writes.
An explicit idle-chat retry row supplies file-mode recovery without composer submission or model work.

### Iteration outcomes

Exact commands (all run from the repository root; `>` logs include stderr via `2>&1`):

| Command | Exit | Retained evidence / interpretation |
|---|---:|---|
| `make build/test_persistence_faults > journal/composition001/redteam/red-build.log 2>&1 && tools/watchdog.sh 300 composition-redteam-red ./build/test_persistence_faults > journal/composition001/redteam/red-test.log 2>&1` | 1 | Original checkpoint + new R1 regression; two assertion failures |
| `make build/test_llm build/test_persistence_faults > journal/composition001/redteam/green-build.log 2>&1 && tools/watchdog.sh 300 composition-redteam-faults ./build/test_persistence_faults > journal/composition001/redteam/green-faults.log 2>&1 && tools/watchdog.sh 300 composition-redteam-chat ./build/test_llm > journal/composition001/redteam/green-chat.log 2>&1` | 1 | Incorrect timestamp-test assumption; chat command not reached |
| `make build/test_llm build/test_persistence_faults > journal/composition001/redteam/iteration2-build.log 2>&1 && tools/watchdog.sh 300 composition-redteam-faults2 ./build/test_persistence_faults > journal/composition001/redteam/iteration2-faults.log 2>&1 && tools/watchdog.sh 300 composition-redteam-chat2 ./build/test_llm > journal/composition001/redteam/iteration2-chat.log 2>&1` | 1 | Test fixture pollution; fixed by giving new adoption trials their own project; chat command not reached |
| `SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make test > journal/composition001/redteam/test.log 2>&1` | 0 | 5,141 chat checks and 3,484 persistence-fault checks; all other suites passed |
| `SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make sanitize > journal/composition001/redteam/sanitize.log 2>&1` | 0 | ASan/UBSan passed; archive/yardstick inputs present and checked; absent optional observer branch skipped |
| `make analyze > journal/composition001/redteam/analyze.log 2>&1` | 0 | Interim analyzer pass; final rerun below covers final production source |
| `SCRIBODO_STORE=off make uitest > journal/composition001/redteam/uitest.log 2>&1` | 0 | Full scripted file-mode UI suite, including new read-only-directory recovery script: 14 passed, 0 failed |
| `tools/watchdog.sh 1800 composition-redteam-mutations python3 tools/test_audit_mutations.py --output /private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo-redteam025.7K029i/mutations > journal/composition001/redteam/mutations.log 2>&1` | 0 | Both controls passed, all 13 mutants rejected by assertions, including the three new save-state controls |

Mutation compilation and execution logs and `results.json` were copied into `journal/composition001/redteam/mutations/`;
compiled binaries/generated sources stay in the temporary harness directory, not the repository.
The original failed attempts stay retained. The successful full test/sanitizer runs precede the final added
worker-overlap regression; rerun them on that final test tree before the remediation commit.

### Isolated database gates

Fresh native services, no real author store: `mktemp -d /private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo-redteam025.XXXXXX`
exited **0**, yielding `/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo-redteam025.7K029i`.
Preflight found both native binaries installed and ports **13471/13472** bindable.
Every database/service command below used these exact environment assignments:

```sh
SCRIBODO_SERVICES=native
SCRIBODO_DB_PORT=13471
SCRIBODO_QDRANT_PORT=13472
SCRIBODO_SECRETS=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo-redteam025.7K029i/secrets
SCRIBODO_LOGS=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo-redteam025.7K029i/logs
```

| Command after that environment prefix | Exit | Evidence |
|---|---:|---|
| `tools/services.sh up > journal/composition001/redteam/services-up.log 2>&1` | 0 | New isolated native store only; credentials remain outside the repo |
| `make -j1 dbtest > journal/composition001/redteam/dbtest.log 2>&1` | 0 | Full DB suite plus new `chat_save_acknowledgements` regression: offline user save, later reply deferral, retry/sequence/window parity and adoption |
| `make -j1 crashtest > journal/composition001/redteam/crashtest.log 2>&1` | 0 | 1,711 checks, zero failures; 151 interruption-matrix rows over 550 seconds. Matrix retained as `journal/composition001/redteam/crash-matrix.tsv`. |
| `make -j1 uitest-db > journal/composition001/redteam/uitest-db.log 2>&1` | 0 | 317 assertions across 16 scripts, zero failures; run serially after crashtest and the file UI suite |
| `tools/services.sh down > journal/composition001/redteam/services-down.log 2>&1` | 0 | Stopped only the isolated test store's native services; no purge or author-service operation |

### Final gate scheduling (completed)

- Final TSan includes the added worker-overlap regression; final analyzer and app-build gates passed.
- DB, crash and DB UI gates ran one at a time, with the isolated environment; cleanup stopped only those
  temporary services. Full test/sanitizer gates were rerun serially.
- Diff/evidence/secret hygiene checks passed. No dependency/model change, real-data migration or model-quality
  claim. Remediations and retained evidence are grouped in the author's requested second commit.

### Final-gate iteration notes

- `make analyze > journal/composition001/redteam/analyze-final.log 2>&1` completed **0**.
- `make all > journal/composition001/redteam/build-final.log 2>&1` completed **0**.
- `make tsan > journal/composition001/redteam/tsan.log 2>&1` completed **0**.
- `make tsan > journal/composition001/redteam/tsan-final.log 2>&1` completed **0** after adding the real
  fake-worker/finished-callback overlap test (normal save and refused-user/deferred-reply arms).
- `SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make test > journal/composition001/redteam/test-final.log 2>&1`
  and the matching `make sanitize` command writing `sanitize-final.log` each exited **2** at the documentation
  evidence gate: prose combining “build” and “run” with a slash was misread as a path. Reworded the prose;
  `SCRIBODO_EVIDENCE_SKIP=1 make evidencecheck > journal/composition001/redteam/evidencecheck.log 2>&1`
  then exited **0**. No evidence exception was added for that prose.
- `SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make sanitize > journal/composition001/redteam/sanitize-final2.log 2>&1`
  exited **2** at the observer yardstick: concurrent full test/sanitizer runs share the yardstick's fixed
  output paths, and this attempt read empty outputs. The runner uses the skip variables only when inputs
  are missing, so these inputs were actually exercised. Run full test/sanitizer gates **serially** from now
  on; the root handoff instructions now state that constraint. Keep this failed log; do not change gold data.
- Secret hygiene scanned modified/untracked files against the three freshly generated isolated-service
  secret values without printing them: **exit 0**, no matches.
- Author follow-up assessed and recommendation accepted: Databricks Labs OntoBricks at `6cd27ce5` is a
  design reference only, not a dependency or code transplant. Its Databricks-restricted licence and
  service-heavy stack do not fit the local lightweight contract. Independently implemented review/stale-source
  and asserted-vs-inferred concepts may inform C09; no new implementation slice or dependency authorized.

### Serial verification outcome

`SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make test > journal/composition001/redteam/test-serial.log 2>&1 && SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make sanitize > journal/composition001/redteam/sanitize-serial.log 2>&1`
completed **0** (both targets **0**), after the final worker-overlap regression was added.
Both logs report **5,161 chat checks** and **3,484 persistence-fault checks**, zero failures, and yardsticks
**717/348/948 lines, byte-identical**. This resolves the concurrent-run yardstick mismatch without changing
production code or the recorded golden outputs. Both `test-final2.log` and `sanitize-final2.log` exited **2**
at the shared-path yardstick and are retained as failed attempts, not counted as passes.

Final relative-link validation across the plan, report, audit, user guide, index, root instructions and
handoff exited **0**; `git diff --check` exited **0**. The temporary services were stopped successfully after
the crash and database UI gates passed.

### Final disposition

R1–R6 are remediated and verified within the report's explicit coverage. The full scripted file UI suite
passed **510 assertions across 29 scripts** (including 14 new refusal/recovery assertions); isolated DB UI
passed **317 across 16 scripts**. Retained app stderr logs are `file-retry-stderr.log` and `db-ui-stderr.log`.
No source changes followed the successful final production-source gates. Test/source limitations remain
explicit in REDTEAM-025: callback preparation's app branch was source-reviewed rather than directly
fault-injected; real-model, native IME/accessibility and ambiguous-write exactly-once behavior are not proven.
F2 and dependency D1/D2 remain queued; the review does not declare the entire product pristine.

`SCRIBODO_EVIDENCE_SKIP=1 make evidencecheck > journal/composition001/redteam/evidencecheck-final.log 2>&1`
completed **0** on the final documentation. Final `git diff --check` and a repeated scan of all
modified/untracked files for broken relative links and the isolated-service secret values completed **0**.
No secret values were printed or committed. The remediation commit is titled
**“Harden chat persistence after adversarial save-state review”**; retrieve its identity from `git log`
rather than maintaining a self-referential hash in this file.

The staged whitespace check found terminal blank lines in the raw TSan and file-UI logs. Their original
bytes are retained in adjacent `.log.gz` files (gzip round-trip verified); only the final empty line was
removed from the readable `.log` copies. No diagnostic, test result or command output content was changed.

## 2026-11-25 — C02 identity investigation

**Tree:** clean `50286c2` before this slice. Author direction: proceed with the tracked P0.
Read CONTRIBUTING and traced file-history writers/readers, path/lock generation, literal-path rename and
set-aside, dated source-name resolution, SQL import, export and project-operation locking.
Boundary map and chosen implementation sequence:
[COMPOSITION-001 revision identity](../docs/COMPOSITION-001-revision-identity.md).

Important discoveries: the importer treats history directory names as logical document names, while
rename/set-aside bypass the private `doc_dir` helper. A one-function path fix would therefore break migration
or strand history. The 200-byte New/Rename limit also exceeds the old key's 199-byte capacity; external and
imported files are not the only relevant entry point. The legacy index contains no full-name owner field,
so missing ownership information must not be inferred from current manuscript contents.

### Red contract evidence

Added `journal/composition001/c02/identity_probe.c` and a small retained Make overlay. Fixture creation is
isolated under the approved temporary directory and includes actual synthetic manuscript files. The
probe uses an unstarted StoreManager (file routing), never a real database, author project or model.

- `TMPDIR=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode make --no-print-directory -f Makefile -f journal/composition001/c02/probe.mk composition-c02-probe C02_PROBE=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo_c02_probe > journal/composition001/c02/red-probe.log 2>&1`
  exited **2**: compilation succeeded; the probe exited 1 after **52 checks, 12 expected failures**.
- `TMPDIR=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode /private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo_c02_probe > journal/composition001/c02/red-probe-direct.log 2>&1`
  exited **1**, confirming an assertion failure rather than a build error/crash.

Both long-name and sanitized-name pairs reproduce history contamination; the ordinary short-name and
legacy-short controls pass. The legacy shared-prefix case demonstrates the absence of an ambiguity refusal.
These are red tests for the required behavior, **not a C02 fix**. The probe is outside normal test targets
until the behavior can be implemented and promoted with coordinated caller changes.

No product source, dependency pin, data format or existing history changed. No migration, DB/crash/UI or
real-model C02 gate has run. The completed C01 gate results do not constitute C02 acceptance.

Baseline-preservation gates launched after the initial design/probe additions:

- `SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make test > journal/composition001/c02/baseline-test.log 2>&1 && SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make sanitize > journal/composition001/c02/baseline-sanitize.log 2>&1`
  completed **0**, both normal targets **0**, run serially. The deliberately red standalone contract probe
  and subsequent native key experiment are not in those targets. These passes preserve the product baseline;
  they do not close F2 or qualify a new revision format.
- `make analyze > journal/composition001/c02/baseline-analyze.log 2>&1` completed **0**:
  zero warnings across 113 product source files. This does not validate the standalone probe or a C02 repair.

### Native representation prototype

Added `journal/composition001/c02/key_probe.c`, not linked into the app or normal tests. It implements
full-byte length-qualified hex keys and a SHA-256-plus-exact-owner comparison for controlled testing.
All fixture writes are under a fresh approved temporary root, not an existing history or database.

`cc -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined journal/composition001/c02/key_probe.c -o /private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo_c02_keys > journal/composition001/c02/key-build.log 2>&1 && TMPDIR=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode /private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo_c02_keys > journal/composition001/c02/key-test.log 2>&1`
completed **0**: **71,648 checks, zero failures**, no compiler/sanitizer diagnostic.

Exercised all byte lengths 1–255, each undersized key-output capacity, malformed decoding, exact Unicode/case
and colon/underscore distinctions, digest-owner mismatch/truncation, disjoint shared-prefix filesystem
leaves, directory rename and archive topology retaining sibling snapshots. Relative-key path arithmetic:
528 bytes maximum for the reversible key, 74 for the digest key. No RAM/latency/migration-cost claim.
See the design note for the working preference and unrun selection gates; the directory moves are not
evidence of crash-consistent application rename/source recovery. The public-API contract probe remains red.

## 2026-11-25 — REDTEAM-026 and requested checkpoint

Author instruction: red-team/remediate/verify the C02 changes, commit, push, then proceed with F2.
Review: [REDTEAM-026](../docs/REDTEAM-026-revision-identity-prototype.md). R1–R4 address fixture
confinement, failed-path output, byte-level snapshot verification, and setup-vs-contract failure reporting.
Original pre-review and red-assertion source are retained, not used as production modules.

Commands and results:

- `cc -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined journal/composition001/c02/key_probe.c -o /private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo_c02_keys_red > journal/composition001/c02/redteam/red-build.log 2>&1 && TMPDIR=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode /private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo_c02_keys_red > journal/composition001/c02/redteam/red-test.log 2>&1`
  exited **1** before repair: **71,660 checks, five assertion failures**. Compiler succeeded; no sanitizer error.
- `python3 journal/composition001/c02/run_key_checks.py --output /private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/c02-redteam-key-controls-1 --tmpdir /private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode > journal/composition001/c02/redteam/key-controls.log 2>&1`
  exited **0** after repair. Unmodified ASan/UBSan control: **101,674 checks, zero failures**, exit 0.
  All **five** mutants failed assertions with exit 1, not crashes or compiler errors. Raw compilation/run
  logs and `results.json` are copied into `journal/composition001/c02/redteam/controls/`; no binaries copied.
- `TMPDIR=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode make --no-print-directory -f Makefile -f journal/composition001/c02/probe.mk composition-c02-probe C02_PROBE=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo_c02_probe_reviewed > journal/composition001/c02/redteam/identity-contract.log 2>&1`
  exited **2** (Make wrapping expected probe exit 1): **52 checks, 12 failures, zero setup/control errors**.
  The F2 product contract is intentionally still red; nothing is silently marked fixed.
- Generated `c02-fixture-failure-control.c` in the approved temporary directory by replacing the single
  `make_project` creation expression with `return false`; no repository source overwritten.
  `TMPDIR=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode make --no-print-directory -f Makefile -f journal/composition001/c02/probe.mk composition-c02-probe IDENTITY_SOURCE=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/c02-fixture-failure-control.c C02_PROBE=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/c02_fixture_failure > journal/composition001/c02/redteam/setup-control-build.log 2>&1`
  exited **2**, compiler succeeded and the generated probe reported setup/control failures.
  Direct confirmation:
  `TMPDIR=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode /private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/c02_fixture_failure > journal/composition001/c02/redteam/setup-control.log 2>&1`
  exited **2**, not expected-contract exit 1.
- `git fetch origin` exited **0**. Current branch is `main`; the two earlier authorized commits `b0f81a3`
  and `50286c2` are ahead of `origin/main` and will accompany the requested checkpoint push.

Final full test/sanitizer (serial), product analyzer, standalone-probe analyzer and final hygiene checks are
pending before commit/push. No new production source/format, services, DB operations or author-data migration.

Second review pass added nonblocking rejection of malformed FIFO snapshots; the exact-byte oracle already
rejects equal-length tampering. Final native negative controls:

- `python3 journal/composition001/c02/run_key_checks.py --output /private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/c02-redteam-key-controls-2 --tmpdir /private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode > journal/composition001/c02/redteam/key-controls-final.log 2>&1`
  exited **0**: control **101,676 checks, zero failures** under ASan/UBSan; five mutants rejected by assertions.
  Final compilation/execution logs and summary retained in `redteam/controls-final/`.
- `cc --analyze -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Xanalyzer -analyzer-output=text journal/composition001/c02/key_probe.c > journal/composition001/c02/redteam/key-analyze.log 2>&1`
  exited **0**, no diagnostic. After the FIFO test:
  `cc --analyze -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Xanalyzer -analyzer-output=text journal/composition001/c02/key_probe.c > journal/composition001/c02/redteam/key-analyze-final.log 2>&1`
  also exited **0**, no diagnostic.
- `cc --analyze -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Isrc -Ithird_party/raylib/src -Xanalyzer -analyzer-output=text journal/composition001/c02/identity_probe.c > journal/composition001/c02/redteam/identity-analyze.log 2>&1`
  exited **0**, no diagnostic.
- Modified/new Markdown relative-link validation exited **0**. Full repository gates still pending.

Final repository gates:

- `SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make test > journal/composition001/c02/redteam/test.log 2>&1 && SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make sanitize > journal/composition001/c02/redteam/sanitize.log 2>&1`
  completed **0**, both targets **0**, serially. These verify the unchanged product baseline and
  documentation gates; the standalone contract is still correctly red and is not in the normal targets.
- `make analyze > journal/composition001/c02/redteam/analyze.log 2>&1` completed **0**:
  no warnings across 113 product sources.

R1–R4 are closed within REDTEAM-026's prototype/evidence scope. No default behavior, schema, service,
dependency or author data changed. The requested commit is titled **“Review revision identity prototype and
strengthen adversarial evidence”**. It will be pushed normally to `origin/main` with the two earlier
authorized commits; no force push or history rewrite. Resume F2 with the shared resolver/enumeration
prototype and explicit legacy eligibility rules, not a writer-only path replacement.
