# AUDIT-001: repository audit and remediation

**Date:** 2026-11-09
**Baseline:** `af57a32` (`REDTEAM-016: red-team the real-manuscript changes and remediate`)
**Committed as:** `6d1a3b3`; subsequent adversarial review is recorded in
[REDTEAM-017](REDTEAM-017-repository-audit.md).
**Scope:** build hygiene, every test suite including the Docker-backed paths,
static analysis, dependency pins and licenses, and repository/remote hygiene.

Measurements below distinguish baseline runs from remediation runs. Logs are
kept under the untracked `build/audit-*.log` paths listed below. File-size
percentages are rounded to one decimal place.

## Findings and remediations

| # | Finding | Measured before | Remediation | Measured after |
|---|---|---|---|---|
| 1 | `compile_flags.txt` omitted the MariaDB include paths, so an editor or language server could not parse any file that includes `mysql.h`. | `clang -fsyntax-only` with the old flags on `src/app/db.c`: `fatal error: 'mysql.h' file not found`. | Added `third_party/mariadb-connector-c/include` and `build/mariadb-cc/include`. | Same command exits 0. |
| 2 | `make uitest-db` failed in `tests/ui/store/6_backup.scribo`: the restored folder is a legacy copy that the import worker moves into the store asynchronously, and the script clicked before that settled. | Exit 2; `6_backup` 10 passed, 5 failed (checks 19, 20, 23, 24, 28); suite 276 passed, 5 failed. Observed on baseline and reproduced before adding the wait. | Added `wait_status "now kept in the project store" 1800` after the restore succeeds. No product behavior changed. | Exit 0; `6_backup` 15 passed, 0 failed; suite 281 passed, 0 failed. |
| 3 | The Clang static analyzer reported 12 findings: null-check gaps after cJSON lookups (`names.c`, `canon_reference.c`, `observer_model.c`, `chat_store.c`), a possible null row in `store_entities.c`, a stream read after EOF in `observer_store.c`, a null entity-list dereference in `main.c`, two dead stores in `chat_store.c`, and a dead store in `main.c`. | 12 warnings across `src/`. | Made every precondition explicit and removed the dead stores. Added `make analyze`, which fails on any diagnostic, and a matching CI job. | `make analyze`: **0 warnings across 82 source files** in 61.6 s. |
| 4 | The local MariaDB and Qdrant containers ran without a no-privilege-escalation policy. | `docker inspect`: `SecurityOpt=[]` for both services. | Added `security_opt: ["no-new-privileges:true"]` to both services in `docker/compose.yml`. | Both containers show `SecurityOpt=[no-new-privileges:true]`; `make dbtest` remains 15,333 checks, 0 failures. |
| 5 | The dependency table omitted three vendored/runtime dependencies and their licenses, and gave no shallow-checkout guidance. | cJSON, llama.cpp and MariaDB Connector/C were absent; `.git/modules` measured 938 MB. | Added cJSON (MIT), llama.cpp (MIT) and MariaDB Connector/C (LGPL-2.1, statically linked) with service and model license notes; added shallow-submodule guidance. | `make dependency-report` verifies every pin and container digest, returns no OSV advisories, and reports the same versions as the table (`cJSON v1.7.19-6-g6d9f244`, llama.cpp `b12111-5-g5851a8e50`, Connector/C `3.4.11`). |
| 6 | The technical-debt register used an old baseline and had no measured entry for the largest file. | Baseline `4060369`; no `main.c` size row. | Updated the baseline and added a dated row. | `src/main.c` is 4,533 lines, 12.2% of `src/` (37,282 C/header lines); accepted for pre-alpha with a concrete split trigger. |
| 7 | Additional analyzer findings in tokenization and recall ranking. The token warning alone does not establish a reachable uninitialized read. | Analyzer warning for `x->hash`; dead store in `recall.c`. | Token allocation is zero-initialized; dead assignment removed. | Both files re-analyze clean. |
| 8 | Three remote-tracking branches were stale. | `fix/persistence-audit-redteam` `bb21814`, `fix/legacy-fork-redteam-final` `a2ae1a7`, `harden-trusted-acceptance` `8f4427b`. | Verified every file each branch touches is byte-identical at its squash commit in `main`. Deleted the first two remote branches; the third was already absent remotely and its tracking ref was pruned. | 31/31, 25/25 and 8/8 files identical at `938e51b`, `b40dcb0` and `8830315`; zero unexplained files. `git ls-remote --heads` confirms all three absent. |

## Test matrix (AUDIT-001 tree, before REDTEAM-017)

| Command | Result |
|---|---|
| `make all` | exit 0, 0 compiler warnings |
| `make test` | 93,520 checks, 0 failures |
| `make analyze` | 0 warnings, 82 source files |
| `make sanitize` | 93,520 checks, 0 failures; no ASan/UBSan diagnostics |
| `make tsan` | 47,239 checks, 0 failures; no ThreadSanitizer warnings |
| `make uitest` | 398 passed, 0 failed |
| `make dbtest` | 15,333 checks, 0 failures |
| `make uitest-db` | 281 passed, 0 failed |
| `make backuptest` | 38 UI assertions (19 before backup, 19 after restore) and 18 shell checks; 0 failed |
| `make remotetest` | 7 passed, 0 failed |
| `make cstest` | 3 passed, 0 failed |
| `make llmtest` | 24 checks, 0 failures against the real BF16 model |
| `make uitest-real` | 35 passed, 0 failed across 3 scripts |
| `make offlinetest` | 134 checks/passes, 0 failed (store, backup, real UI scripts, real embedder, real model) |
| `make dependency-report` | all pins and digests verified; no OSV advisories |
| Documentation links | 157 relative links across 115 Markdown files, 0 broken |
| `git fsck --no-progress` | no errors (dangling blobs only) |

## Final-review regression

Final review added allocation-failure regressions for an unterminated metadata
line and an unterminated observer line in chat JSONL. The initial analyzer
cleanup incorrectly relied on EOF to detect a parsing failure: `getline` can
already set EOF while returning a final line without a newline. The regression
produced **26 failures in 1,037 checks** before the correction and **0 failures
in 1,037 checks** afterward. A separate parsing-success flag now preserves the
failure regardless of stream EOF state. Evidence:
`build/audit-unterminated-before.log` and `build/audit-unterminated-after.log`.

## Accepted on record

`src/main.c` remains a single composition-root translation unit at 4,533 lines
(12.2% of `src/`). The P1 coordination split moved worker ownership out; what
remains is the single-window layout builder and scripted-host adapters. It is
accepted for pre-alpha and should be split only when a feature next touches more
than three ownership areas, or if the file exceeds 5,000 lines. This is
measured debt, not an unexamined exception.

## Evidence

The corrected tree was rerun sequentially through all 14 targets in the matrix
(excluding the separately run dependency report). Every target exited 0.
Machine-readable exit codes, durations and assertion totals are in
`build/audit-verified-results.json`; individual logs are
`build/audit-verified-<target>.log`, where `<target>` is `all`, `analyze`, `test`,
`sanitize`, `tsan`, `uitest`, `dbtest`, `uitest-db`, `backuptest`, `remotetest`,
`cstest`, `llmtest`, `uitest-real` or `offlinetest`.

Other final checks:

- `build/audit-dependency-report-final.log`
- `build/audit-links-final.json`
- `build/audit-fsck-final.log`

The CI analyzer job was added but has not been executed on GitHub Actions in
this audit session; the matching local target passed. These measurements were
recorded before committing the audit as `6d1a3b3`.
