# Legacy import and fork recovery remediation

Date: 2026-11-06. Baseline: `938e51b`. Scope: the outstanding legacy file-reader,
copy/fork retry, and real-model/offline validation concerns from the persistence
audit. This is a scoped remediation record, not a claim of universal correctness.

## Reproduced failures and fixes

| Failure | Remediation | Durable regression |
|---|---|---|
| Unreadable notes/canon/findings or revision indexes looked empty | Checked file-backend APIs propagate I/O and allocation errors through routing, writes and import | `file_history_read_failures`, `legacy_import_read_failures` |
| Unreadable/damaged revision snapshots were skipped during migration | Abort the complete transaction; preserve file authority; permit repair/retry | Seven legacy-import failure cases, including a bad snapshot hash, with zero partially committed document rows |
| Parse allocation failure looked like a crash-torn JSON line | Thread-local allocation-error tracking in the cJSON compilation wrapper, read through the serialized parser boundary | `test_persistence_faults` fails allocations in representative record/revision/chat reads |
| JSON builders could report success with omitted fields after allocation failure | Check every required field, source and attachment allocation before writing a complete record | Writer allocation-failure cases; no partial authoritative entry is accepted |
| Failed forks appended partial exports to the copy on retry | Export in a consistent repeatable-read transaction into journal-owned staging, then import from the completed snapshot | `fork_retry_snapshot`: failed export, blocked registration, unreadable staged note, store restart, later source changes, successful retry without duplicates |
| Copied `import-pending` could select the original project's row | Resume only a pending row whose canonical owner path is this folder; write pending state atomically | Foreign pending ID in the fork retry fixture; source and copy remain independent |
| Equal snapshot filenames in different documents could cross-link provenance | Match document identity and revision filename, following renames at the revision's time | `import_revision_identity` creates identical filenames for two different documents and checks the source/revision document IDs agree |
| Real-model/offline acceptance had not been run | Build the pinned runtime locally using cached CMake and existing BF16/embedding assets; disable unused web-UI downloads | Real-model end-to-end and sandboxed offline acceptance |

Read/import failures, fork duplication, allocation-related read omissions, and
both identity failures were observed as failing assertions before their fixes.
Required-field writer guards were additionally verified by mutation testing.
The isolated allocation suite has 680 checks on this dated run, covering both
single recoverable allocation failures and sustained failures from each selected
allocation onward. This includes setup and output/cleanup assertions, not 680
distinct allocation sites.

## Retry and preservation contracts

- Missing history is a clean empty result. Unreadable, special, changing or
  raw-NUL-containing input is an error. JSON string NULs are refused rather than
  silently truncating C strings. Existing crash-torn-line and safe legacy-field
  recovery behavior remains supported.
- A failed checked read returns no partial records or revisions. Writers do not
  derive a new version from a failed read. Import covers documents and all
  authoritative repositories in one transaction and rolls back on failure.
- `.scribodo/fork.json` records the source, target, completion state and metadata
  fingerprint of `.scribodo/fork-snapshot/`. Incomplete exports can be rebuilt
  only in that journal-owned directory. An unrecognized staging directory is
  preserved, not removed. Completed snapshots are reused, not appended to or
  refreshed with later source messages.
- A committed fork archives the snapshot in a unique, time-ordered `legacy-*`
  directory before removing pending state. Copied post-commit journals can finish
  cleanup. A changed/missing snapshot fails closed. Metadata fingerprints are
  crash/retry checks, not protection against an adversary rewriting local files.
- The cJSON checkout and allocator hooks are unchanged. Its implementation is
  compiled through `src/core/json_vendor.c`; the default system allocator is
  preserved, with a thread-local failure flag. Fault injection is confined to a
  separate test binary. Runtime allocator-hook or locale changes remain unsupported.

## Mutation checks

Temporary objects were compiled and linked into the real test binaries. Each of
these seven mutations produced assertion failures; compilation failures, crashes
and timeouts were not counted as catches:

1. Restore the file-record router's error-collapsing count API.
2. Ignore cJSON's allocation-error flag and treat OOM as malformed input.
3. Skip an unreadable revision snapshot during import.
4. Rebuild a completed fork snapshot on every retry.
5. Resume any pending project ID without checking folder ownership.
6. Match source revisions by filename without matching the document.
7. Serialize a partially built record after a required JSON field failed.

Working sources and pinned dependencies were not modified by the mutation runs.

## Validation

- `make test`: includes permission/FIFO regressions and isolated allocation faults.
- `make sanitize`: the same headless/fault suites under ASan/UBSan.
- `make tsan`: concurrent parsing, chat, observer and owned worker paths.
- `make dbtest`, also under ASan/UBSan: transaction rollback, fork retry/restart,
  identity, ownership and existing MariaDB/Qdrant integration.
- `make uitest uitest-db backuptest remotetest`: native UI and recovery acceptance.
- `make llmtest`: the pinned BF16 model, streaming/authentication and crash recovery.
- `make offlinetest`: external-host denial and loopback controls, store/backup
  workflows, real chat/edit/observer UI, the real embedding model, and the real
  model end-to-end suite inside the network-restricted sandbox.
- `make dependency-report` and `git diff --check`.

All listed suites passed on the final production code. The real-model/offline
suite passed twice during this remediation, including the final-code run. The
expanded one-shot/sustained allocation suite subsequently passed all 680 checks
both normally and under ASan/UBSan. Seven final scratch mutations were caught by
assertions, and `git diff --check` passed. This closes the three stated concerns
within the documented scope; it does not assert that every possible application
or filesystem failure has been exhaustively tested.
