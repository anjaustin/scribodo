# Persistence remediation red-team

Date: 2026-11-06. Baseline: `8830315` plus the uncommitted persistence audit
remediation. Scope: the remediation diff, concurrent file parsing, migration
error handling, explicit exports, and backup creation/verification. This is a
scoped review, not a proof that the entire application is defect-free.

## Findings and remediations

| Finding | Remediation | Regression evidence |
|---|---|---|
| Concurrent tokenization shared state across chats/revision indexes | Caller-owned `strtok_r` state at every application call site | Concurrent readers failed before the original fix; now checked under ASan/UBSan and TSan |
| cJSON writes shared parser error state even on success | All application/test parsing uses the serialized `core/json.c` boundary; pinned dependency unchanged | Expanded TSan test reported the race before the boundary was introduced |
| Migration ignored failed chat enumeration/loading | Abort/rollback, preserve file authority, permit repair and retry | Invalid timestamp, unreadable log/directory, zero partially committed conversations |
| Export treated record read failure as an empty result | Checked record list for all record kinds | Corrupt note/canon/finding status must fail export |
| Backup creation preserved links that restore rejects | Refuse links/special entries before copying; verify output before success; remove failed output | Regular, dangling and directory symlinks; FIFO; link introduced during copying |
| Verification skipped unreadable extra directories | Propagate `find` failure and Python walk errors | Unlisted file in a mode-000 directory previously verified successfully |
| Malformed recognized messages or NULs silently disappeared | All-or-nothing recognized-message validation; reject raw and decoded NUL, including JSON `\u0000` | Invalid role/content/timestamp and NUL fixtures failed against the first remediation; literal `\\u0000` and missing legacy timestamps remain valid |
| Chat metadata was lost when exporting to files | Persist/read truncation flags and full fractional timestamps | File and database-export round trips |
| Fractional chat times could sort after an actually later rename rounded to milliseconds | Rename serialization uses microsecond precision too | Immediate rename/source checks and the existing import/rename test |
| Retrying partial exports appended duplicate history | Require fresh structured metadata, serialize exports with an exclusive transition lock | Both partial and completed destination reuse are refused without appending |

Backup checksums include nested author files named `SHA256SUMS`, rather than
excluding every file with the checksum manifest's basename. Destinations inside
the source trees are refused; existing destinations, including dangling symlinks,
remain untouched.

## Mutation validation

Eight C mutations were compiled into temporary objects and linked into the actual
test binaries; two shell mutations used temporary copies of the backup script.
The working sources and pinned dependencies were not modified by these checks.
Each of these ten valid mutations produced an assertion failure; no crash,
build failure, or watchdog timeout was counted as a catch:

1. Remove raw-NUL refusal.
2. Stop persisting the truncation flag.
3. Round message timestamps back to whole seconds.
4. Round rename timestamps back to milliseconds.
5. Restore the error-collapsing record-list call during export.
6. Bypass the fresh-export destination check.
7. Skip verification before reporting backup success.
8. Ignore traversal errors in both backup-verification passes.
9. Allow escaped NULs through the file reader.
10. Treat a wrong-type timestamp as an omitted legacy timestamp.

## Contracts and limits

- Explicit `store_export_project` output is a fresh destination, not an append or
  merge operation. Failed output can be partial; retry in a new destination.
  In-app backups remove their own temporary export on failure.
- Crash-torn JSON lines retain the existing recovery behavior; malformed
  recognized message objects are not silently skipped.
- Backup checksums detect damage, not a maliciously rewritten backup. Restore
  only backups you trust. Concurrent hostile filesystem substitution is not a
  supported trust boundary.
- JSON allocator hooks and process locale must remain unchanged while workers
  run. Do not bypass `json_parse` or use cJSON's shared error-pointer API.
- Real-model/offline acceptance requires a built local `llama-server` and model
  assets. The runtime is not present in this checkout; those suites are not
  claimed as validated by this review.

## Verification

The regression suites are `make test`, `make sanitize`, `make tsan`, and
`make dbtest`. Full acceptance uses `make all uitest uitest-db backuptest
remotetest`. Database integration is additionally run under ASan/UBSan. Dependency
pins and advisories are checked with `make dependency-report`.

All of the above available suites passed on the final code, including the full
UI/store UI and disaster-recovery runs, sanitized database integration, and the
dependency report. `git diff --check` and shell syntax checks also passed. The
final scoped review has no remaining confirmed findings. Real-model/offline
acceptance remains unrun for the prerequisite reason above.
