# Failure-path audit remediation

**Date:** 2026-11-06

**Baseline:** `51941d3`

**Scope:** the three local audit findings, their immediate failure paths and
regression coverage. The existing sample-document edit is unrelated and excluded.

## Findings and fixes

### Embedding allocation failure

When JSON serialization returned NULL, a short-circuited request left an
uninitialized `HttpResult`. Error formatting and cleanup then consumed invalid
stack state. A fault-injected probe reproduced an ASan stack-buffer-overflow.

The request now checks each JSON construction/insertion and serialization step,
reports allocation failure before transport, and initializes its result. Qdrant
requests also refuse a failed serialization rather than send an empty body.

Adjacent response checks now validate the entire batch before writing output:
the required vector count, unique integral indices in range, array dimensions
and finite, float-representable numeric components. Reordered batches and older
servers omitting indices remain supported. No partial output is published on
failure.

### Refused editor replacement deleting the passage

stb deletes a selection before asking the editor to insert its replacement. If
gap growth failed, the API returned false after deleting the original passage
and disturbing selection, anchors and history.

Range replacement, paste and typing now reserve net growth before changing those
states. The allocation regression sweeps both one-shot and sustained failures,
including UTF-8 conversion, and preserves an existing redo branch on refusal.
Successful edits under memory pressure can still lose undo history if recording
the edit fails; the existing `undo_lost()` policy deliberately drops inconsistent
history rather than replay it against changed text. This review does not claim
that undo recording can never fail.

### Startup hanging on a FIFO configuration

An owner-only FIFO passed the configuration ownership check and blocked in
`fopen()`, which runs on the startup thread.

Configuration and key reads now open nonblocking, validate the actual descriptor
as a regular file with the required ownership/permissions, enforce byte limits,
and check complete/stable reads before parsing. Raw NULs, truncated/oversized
credentials and additional key lines are refused; an ordinary trailing CRLF and
a maximum-length key are supported. An absent configuration remains normal.

## Red/green evidence

Regressions were run against production code before remediation:

| Regression | Baseline result |
|---|---|
| Editor allocation sweep | 60 assertion failures in 157 checks |
| Embedding allocation/response checks | 22 assertion failures in 8,306 checks |
| Bounded FIFO configuration reads | 2 assertion failures in 57 checks |

The editor harness's initial missing font setup caused a crash; that run was
discarded and the corrected harness produced the assertion-based result above.
The final suites also cover additional cases beyond those initial regressions.

## Mutation checks

Each mutation was compiled into a separate executable from scratch source under
`build/remediation-mutations/`; working-tree production sources were not replaced.
All nine executables exited with assertion failures (exit 1), not a crash or
watchdog timeout:

1. Remove range-replacement growth preflight.
2. Remove paste growth preflight.
3. Remove typing growth preflight.
4. Permit duplicate embedding indices.
5. Skip embedding-component numeric/finite validation.
6. Permit a missing allocated input item to reach serialization.
7. Restore blocking remote-config/key opens.
8. Remove the remote file-size limit.
9. Ignore embedded NULs in remote files.

## Initial remediation verification

- Fresh application build and complete headless suite: passed (22,590 C checks
  and 11 Python tests on this run).
- ASan/UBSan, including both allocation suites: passed.
- TSan, including the embedding transport/allocation suite: passed.
- Normal and sanitized MariaDB/Qdrant integration: passed.
- Dependency pin, registry and OSV report: passed.
- Clang static analysis of the three changed production modules: no diagnostics.
- Nine scratch mutation checks: all caught by assertions.
- Native UI, store UI, backup/restore, stand-in remote provider, case-sensitive
  rename and offline real-model acceptance: all passed in one serialized run,
  including real chat, reviewed edits, observer, embeddings and the live adapter
  with outside-network access denied.

Raw evidence is kept locally in `build/remediation-*.log`. This establishes the
documented failure contracts, not exhaustive proof against every allocation,
filesystem or hardware failure. Local filesystem metadata is not an adversarial
authentication boundary.

## Adversarial follow-up on `720f5b0`

### Complete bytes were not necessarily complete JSON

The regular-file reader checked the complete file, but cJSON's default parser
still accepted a valid JSON prefix followed by garbage or another JSON value.
Escaped NULs could also truncate values or alias field names after decoding. The
same problem affected embedding bodies, including a raw NUL followed by hidden
bytes in a complete HTTP response.

The initial adversarial tests produced eight remote configuration assertions
and four embedding assertions on this boundary, before remediation. A separate
`json_parse_complete(text, n)` boundary now uses the existing parser mutex,
consumes an exact byte span and rejects raw/decoded NULs or trailing non-whitespace
bytes. Exact spans do not require an accessible terminator; ASan exercises that
contract. Literal escaped backslashes and trailing JSON whitespace remain valid.
Only the configuration and embedding adapters adopt it; legacy crash-torn log
parsing is intentionally unchanged.

Configuration parse failures now always supply a reason for over-width model or
URL fields, and over-width configuration paths fail rather than silently
truncate. A dangling configuration symlink is an error, not silently equivalent
to an absent file.

### Service-secret exclusion missed configured roots and hard links

The old exclusion used the default service pathname and a literal path substring.
Three new assertions reproduced acceptance of a fake service key from a custom
`SCRIBODO_SECRETS` root, its symlink alias and a hard link outside the root.

The check now inspects the actual opened key descriptor, canonical default and
configured service roots, and device/inode identities of the known credential
files (`db_password`, `db_root_password`, `qdrant_api_key`, `qdrant.env`). It fails
closed if required metadata cannot be checked. No service credential contents
are read. This protects against accidental selection and ordinary aliases, not
deliberate same-user filesystem tampering or an undiscovered future credential
filename outside the service directory.

### Follow-up mutation and verification evidence

The original nine compiled mutation checks are retained. Six additional scratch
mutations remove complete-input consumption, decoded-NUL rejection, the service
secret guard, descriptor identity comparison, dangling-link diagnosis or the
configuration-path length guard. The final compiled rerun and serialized native
acceptance evidence is recorded below. Raw follow-up evidence is in
`build/redteam2-*.log`.

- All 15 mutations were compiled and caught by assertions (exit 1); none was
  counted as caught because of a build failure, crash or watchdog timeout.
- Fresh application/headless build: 22,639 C checks and 11 Python tests passed.
- ASan/UBSan, TSan, normal and sanitized database integration: passed.
- Dependency checks and static analysis of all four changed production modules:
  passed; no analyzer diagnostics.
- An ASan/UBSan probe placed 20,000 randomized non-terminated spans immediately
  before an inaccessible guard page, plus valid JSON/trailing-content controls;
  it passed without an out-of-span read or sanitizer report.
- Final serialized native UI, store UI, backup/restore, stand-in remote provider,
  case-sensitive rename and offline real-model acceptance: all passed (42 script
  runs, 628 expectations), including live BF16 chat and real embeddings with
  outside-network access denied.

A second source-level pass reviewed buffer reservation before stb mutation,
request/response allocation ownership, bounded parsing and parser locking,
descriptor validation and service-root/path alias handling. It found no further
confirmed defect within this scope. The residual limits above still apply.
