# REDTEAM-017: adversarial review of the repository-audit changes

**Date:** 2026-11-09

**Reviewed commit:** `6d1a3b3` (`AUDIT-001: remediate repository audit and verify every acceptance suite`)

## Scope and method

Review the complete AUDIT-001 diff, not just the passing test totals: analyzer
coverage and failure semantics, parser failure propagation at EOF, tokenization
boundaries, entity-list/detail allocation guards, restore synchronization,
container initialization, dependency documentation and evidence claims.

Use negative controls and deliberately broken implementations where possible.
Compiler failures, crashes and timeouts do not count as killed mutants. This
review does not claim that passing tests proves an absence of all defects, nor
does it erase the accepted `main.c` composition-root debt.

Iteration 1 found R1–R4 through source review, a failing inventory regression and
mutation controls. Iteration 2 exercised a clean shallow checkout and found R5's
overly strong wording. After those remediations, the final pass repeats the full
suite matrix, analyzer-gate tests and all ten mutation controls.

## Findings and remediations

### R1 — analyzer coverage omitted Objective-C (P2, fixed)

The new target claimed every first-party source but selected only `*.c`.
`src/platform/dialogs_mac.m` was never analyzed. An independent inventory test
failed against the reviewed commit: **1 failed of 8 tests**, identifying exactly
that file. The target now selects C and Objective-C sources, while retaining
the two documented vendored-amalgamation exclusions.

`make analyzetest` now runs eight gate tests: source inventory, a clean source,
a real null-dereference warning, a compile error, a missing analyzer, a silent
nonzero tool exit, a diagnostic without a `warning:` prefix, and a clean rerun
after a diagnostic-bearing run. All eight pass. CI runs this gate suite before
the full-source analyzer. Direct full analysis is now **83 files, 0 warnings**.

Evidence: `build/redteam017-analyze-gate-before.log`,
`build/redteam017-analyze-gate-after.log`, `build/redteam017-analyze.log`.

### R2 — chat regression checked failure handling but not restored values (P2, fixed)

The new unterminated-JSONL test asserted successful listing and a count of one,
but did not check that the title and observer-ignore flag survived parsing.
Added explicit success-path assertions for both fixtures. Mutants that silently
drop the title or the observer flag now fail **2** and **1** assertions,
respectively. Reintroducing the EOF/allocation-failure bug fails **26** assertions.

### R3 — signature-reader boundary coverage was indirect (P2, fixed)

The changed stream-read logic had integration coverage for a valid signature,
but no direct boundary/error matrix. Added missing-file, empty-file, lower- and
upper-case 16-character hexadecimal, short lengths (1 and 15), extra-byte,
trailing-newline, non-hex, embedded-NUL and directory/read-error cases.

The expanded persistence suite passes **1,095 checks, 0 failures**. Mutants that
accept trailing bytes, accept non-hex data or ignore read errors fail **2**,
**2** and **1** assertions, respectively.

### R4 — tokenizer comment overstated what the warning established (P3, fixed)

The comment implied that unused token capacity could legitimately be read and
that zero-initialization established the safety of future reads. The diff
algorithm is bounded by emitted token counts; an analyzer warning alone does
not establish a reachable uninitialized read. Reworded the comment to state
only the initialization and token-count invariants, retaining `calloc`.
Added explicit empty-to-empty and nonempty-to-empty round-trip checks.

### R5 — shallow-clone wording promised only pinned revisions (P3, fixed)

The README said the command fetched only pinned revisions. A real clone showed
all seven submodules were shallow, but six retained two reachable commits
(upstream tip plus the separately fetched pin); only cJSON retained one. Changed
the wording to promise shallow histories, not exclusively pinned objects, and
linked the measured comparison below instead of presenting a checkout-specific
size as a universal property.

## Mutation verification

Run `python3 tools/test_audit_mutations.py`. Generated sources, binaries and logs
remain under `build/audit-mutations/`; tracked sources are never mutated.
The harness derives the compiler command from the actual Make target and
requires a passing unchanged control plus a normal test failure for each mutant.

| Variant | Test failures | Result |
|---|---:|---|
| Unchanged control | 0 of 1,095 checks | passed |
| EOF hides JSON allocation failure | 26 | killed |
| Chat title not restored | 2 | killed |
| Observer flag not restored | 1 | killed |
| Extra signature bytes accepted | 2 | killed |
| Non-hex signature accepted | 2 | killed |
| Signature read error accepted | 1 | killed |

The gate suite is itself mutation-tested in a generated source layout, without
changing the real Makefile:

| Gate variant | Failing tests (of 8) | Result |
|---|---:|---|
| Unchanged gate | 0 | passed |
| Objective-C omitted | 1 | killed |
| Diagnostic log ignored | 3 | killed |
| Tool exit status ignored | 1 | killed |
| Stale diagnostic log appended instead of replaced | 1 | killed |

Total: **10 mutants killed, 0 survived; both unchanged controls passed**.

Evidence: `build/redteam017-mutations.log` and
`build/audit-mutations/results.json`.

## Additional adversarial checks

- Started both services under a uniquely named Compose project, with fresh
  project-owned volumes and ephemeral loopback ports. Both became healthy with
  `no-new-privileges:true`; MariaDB accepted a table creation, insert and read,
  and Qdrant answered a request carrying its API key. Removed only that temporary
  project's containers, network and volumes afterward. Existing services/data
  were not replaced. Evidence: `build/redteam017-fresh-containers.log`.
- Independently read `/proc/1/status` in both regular service containers:
  **`NoNewPrivs: 1`** in each, confirming the runtime policy rather than only the
  Compose configuration. Evidence: `build/redteam017-runtime-hardening.json`.
- Reviewed the entity guards against empty lists, allocation failure and invalid
  DB-cell outcomes: the existing failure flag still propagates; no change to
  successful row counts or data ownership was found.
- Reviewed the restore wait against the worker-poll/frame ordering: it waits
  for completed import, and the script runner inserts a fresh batch before
  subsequent clicks. The full DB/UI and offline suites exercise that path.

### Clean shallow checkout

Executed the README's clone command into `build/redteam017-shallow`, applied the
reviewed commit and tracked follow-up patch, and copied the new analyzer test.
Ran `make all analyzetest analyze ARCH_FLAGS=` there, with no reused compiled
libraries or generated headers. The application built, all eight gate tests
passed and all 83 source files analyzed cleanly. This exercises the local
equivalent of CI's portable-architecture build, not GitHub's runner itself.

Measured `.git/modules` disk usage with `du -sk`: **960,220 KiB** in the existing
full-history checkout and **112,308 KiB** in the shallow checkout. Sizes describe
these two checkouts on this date, not a guaranteed clone size. Every submodule
reported `--is-shallow-repository=true`.

Evidence: `build/redteam017-shallow.log` and
`build/redteam017-shallow-inventory.json`.

## Final verification

All **16 targets exited 0**, with **0 test failures and 0 compiler/analyzer
warnings**, in a measured **742.005 seconds** of sequential wall time.

| Target | Result |
|---|---|
| `all` | build passed, 0 compiler warnings |
| `analyzetest` | 8 Python tests passed |
| `test` | 93,585 C checks and 11 Python tests passed |
| `sanitize` | 93,585 C checks and 11 Python tests passed; no ASan/UBSan diagnostics |
| `tsan` | 47,239 checks passed; no ThreadSanitizer diagnostics |
| `uitest` | 398 passed |
| `dbtest` | 15,333 checks passed |
| `uitest-db` | 281 passed |
| `backuptest` | 38 UI assertions and 18 shell checks passed |
| `remotetest` | 7 UI assertions plus provider-side checks passed |
| `cstest` | 3 passed |
| `llmtest` | 24 real-model checks passed |
| `uitest-real` | 35 passed |
| `offlinetest` | 134 checks/passes; real models and store exercised under the network sandbox |
| `analyze` | 83 C/Objective-C translation units, 0 findings |
| `dependency-report` | pins and container digests verified; no OSV advisories returned |

Evidence: `build/redteam017-verified-results.json` and
`build/redteam017-verified-<target>.log`. The 65-check increase from AUDIT-001's
unit total comes from the strengthened chat assertions, signature-reader matrix
and empty-diff cases. The analyzer gate tests are a separate target and also run
in its CI job.

**Disposition:** R1–R5 are closed; the final scoped review found no further open
findings. The accepted composition-root debt remains explicitly recorded, not
silently scored away. This report records local verification, including a clean
shallow checkout; it does not claim a GitHub Actions run has completed.

The run evidence is preserved in `scribodo-evidence-2026-11-09-audit.tar.gz`;
see `docs/EVIDENCE.md` for the archive location, manifest and checksum.

## Hosted-CI iteration after the first push

The initial push of `69eee28` exposed another finding, so the local disposition
above was not the end of the review. Hosted CI run `42690786517` passed build,
unit, ASan/UBSan and TSan jobs, but the analyzer job failed with one warning at
`src/llm/remote.c:195`: a potentially uninitialized byte in the API-key suffix
loop. The analyzer-gate tests passed on that runner. Its trace used the
`Xcode_16.4.app` SDK; local analysis with Apple Clang 21 had not reported this
path. No checker was disabled and the failure gate was not weakened.

### R6 — rejected API keys still entered suffix validation (P2, fixed; hosted rerun required)

The trace continued into suffix validation after an empty key had already set
`ok=false`. Made the loop conditional on `ok`, so rejection stops subsequent
validation and no bytes are examined on that path. Existing accepted-key rules
remain in force. A warning alone does not establish a reachable runtime
uninitialized read; the correction makes the validation precondition explicit.

Added a 12-case key-line matrix, run with both a normal error buffer and a
zero-length one: empty/CR/LF-only keys, an ordinary key, permitted trailing line
endings, a second key line, and invalid embedded/trailing characters. Rejected
keys must leave the entire output credential buffer zeroed. The remote suite
passes **149 checks, 0 failures**. CI now prints the analyzer version so future
toolchain differences are measured rather than inferred.

The initial pushed commit's trusted acceptance run `42690786966` passed its
database integration and both UI suites. Recovery acceptance is schedule/manual
only and was skipped on that push; it was covered by the local matrix above.

Evidence: `build/redteam017-hosted-ci-failure.log`,
`build/redteam017-hosted-ci-jobs.json`,
`build/redteam017-trusted-ci-result.json` and
`build/redteam017-remote-key-boundaries.log`. The correction's full local rerun
is recorded in `build/redteam017-ci-fix-results.json` and the corresponding
`build/redteam017-ci-fix-<target>.log` files; hosted confirmation follows the
next push.

### R7 — cold Connector/C configuration passed an unused option (P3, fixed)

The same hosted log showed a CMake warning that `CLIENT_PLUGIN_REMOTE_IO` was
unused. This pre-existing build option is not consulted when `WITH_CURL=OFF`:
the pinned dependency's `plugins/io/CMakeLists.txt` registers `remote_io` only
inside `IF (WITH_CURL)`. Removed the redundant option, retaining `WITH_CURL=OFF`.

A fresh configure and `mariadbclient` build under
`build/redteam017-mariadb-clean` completed with **0 CMake warnings and 0 compiler
warnings**. The resulting cache explicitly records `WITH_CURL=OFF`. Evidence:
`build/redteam017-mariadb-clean.log` and
`build/redteam017-mariadb-clean-result.json`.

### Verification of the hosted-CI corrections

Repeated all 16 targets after the key-validation correction. Every target exited
0, with 0 failures and 0 compiler/analyzer warnings, in **729.310 seconds** of
sequential wall time. Updated totals: **93,641 C checks** for both `test` and
`sanitize` (plus 11 Python tests each), **47,295 TSan checks**, and **149 remote
suite checks**. All other counts match the earlier matrix; analysis remains
**83 files, 0 findings**. No sanitizer diagnostics were reported.

Repeated the mutation controls: **10 killed, 0 survived; both unchanged controls
passed**. The cold Connector/C build separately verifies the cleaned-up CMake
recipe, without relying on the existing shared-library build cache.

The hosted analyzer result must still be confirmed after this corrective push;
local Clang 21 passing is not substituted for that confirmation. Evidence:
`build/redteam017-ci-fix-results.json`, `build/redteam017-ci-fix-*.log`, and
`build/redteam017-ci-fix-mutations.log`.

### Hosted confirmation (recorded 2026-11-09 by AUDIT-002)

The corrective push `278259c` ran hosted CI as run `42693799204`. Its Clang
static analyzer job passed on the GitHub runner, which closes R6's pending
hosted confirmation. The same run's build/unit and ASan/UBSan jobs failed in
`tests/test_memory_vectors.c` (the private memory vector allocation sweep);
that failure was not recorded at the time and is examined in
[AUDIT-002](AUDIT-002-repository-audit.md).
