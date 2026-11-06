# Fork marker and snapshot exhaustion follow-up

Date: 2026-11-06. Reviewed baseline: `8d3494b`.

## Findings and remediation

1. **Fork authority gap.** The old fork temporarily renamed `store.json` away.
   A process death in that interval could expose the copy as an unmarked
   file-backed project, although its authoritative export was in private staging.
   Forks now retain the copy marker through registration and import. Only the
   final atomic marker replacement changes authority. Errors no longer need a
   marker-restoration rename. The regression puts an unrelated directory at
   `store.json.set-aside`: the baseline fails; the fixed fork succeeds and leaves
   that directory alone. Failed exports/registration also remain paused and
   refuse chat writes. This tests the marker-withdrawal dependency and retry
   invariant; it is not an exhaustive power-loss test at every syscall.

2. **Snapshot name exhaustion overwrote existing history.** After finding 1,000
   occupied timestamp/origin names, revision recording broke out of its search
   and atomically replaced the last existing snapshot. The new regression
   reproduced the overwrite. Exhaustion now returns failure before writing any
   snapshot/index entry. Name checks use `lstat`, treating dangling symlinks as
   occupied and inspection errors as failures. A bounded search is deliberate:
   refusing a save to History is preferable to overwriting an immutable snapshot.

3. **Snapshot reuse trusted only the index.** A matching index hash reported
   successful snapshot capture or duplicate History recording even if the actual
   file was missing or damaged. A subsequent review iteration reproduced both
   false successes. Reuse now reads the candidate under the existing revision
   lock and requires exact content equality. Failure returns no snapshot filename.
   Tests cover missing/damaged latest snapshots, successful repair, and reuse of
   a missing older snapshot after newer History has been recorded.

## Adversarial validation

- `snapshot_name_exhaustion` fills all 1,000 candidates, checks failure and the
  unchanged final snapshot, and verifies that the revision index is unchanged.
- `fork_retry_snapshot` exercises retained-marker behavior alongside the existing
  incomplete export, immutable retry, foreign pending owner, restart and cleanup
  cases.
- `import_revision_identity` additionally round-trips revisions through export
  and import with sequence order opposing wall-clock time. This passed without
  an additional production change.
- Ten temporary compiled mutations were caught by assertions: the seven from
  red-team 011, restoring the exhaustion overwrite, and restoring baseline fork
  marker withdrawal, plus bypassing snapshot-content verification.
  Crashes/timeouts were not counted as catches. The mutation
  harness does not edit working sources.

## Final validation

After the second remediation iteration, all of the following passed:

- `make test dbtest`: 3,552 adapter checks, 680 allocation-fault checks,
  590 repository checks and 37 database core checks, plus the other headless suites.
- `make sanitize`, sanitized `dbtest`, and `make tsan`.
- `make all uitest uitest-db backuptest remotetest offlinetest`: native UI,
  copy/fork, backup/restore, stand-in remote provider, network denial/loopback
  controls, real chat/edit/observer, real embeddings and model crash recovery.
- Ten compiled mutations, dependency reporting, and diff whitespace checks.

## Limits

This review covers the changed persistence/recovery paths and adjacent snapshot
allocation behavior, not every application feature. Filesystem fingerprints are
not a hostile-tampering boundary. These tests do not prove absence of all races
or emulate every hardware/power failure. No new unresolved finding remained in
the final scoped review; that is not a universal "defect-free" claim.
