# AUDIT-002: repository audit after the memory and diagnostics work

**Date:** 2026-11-09
**Baseline:** `6c243d5` (`Add live-path and manifold diagnostics for answer reliability`)
**Previous audit:** [AUDIT-001](AUDIT-001-repository-audit.md) and
[REDTEAM-017](REDTEAM-017-repository-audit.md) at `af57a32`; four commits have
landed since (`1bfd022`, `66cf105`, `2a3211f`, `6c243d5`).
**Scope:** every verification target including the Docker-backed and real-model
paths, a clean rebuild, static analysis, the Python diagnostic tooling (never
audited before), dependency pins, documentation and remote/hosted-CI hygiene.

Measurements distinguish the baseline tree from the remediated tree. Logs are
kept under the untracked `build/audit002-*` paths listed at the end.

## Baseline matrix (unchanged tree, before any remediation)

All 17 targets exited 0 in a measured **744.151 s** of sequential wall time
(`build/audit002-results.json`).

| Target | Result |
|---|---|
| `all` | incremental build, nothing to compile; a separate clean rebuild into `build/audit002-clean` ran 93 compile commands with **0 compiler warnings** |
| `analyzetest` | 8 Python gate tests passed |
| `test` | **95,597 C checks** in 26 binaries and **59 Python tests** in 10 files, 0 failures |
| `sanitize` | same 95,597 checks and 59 Python tests, 0 failures; 0 ASan/UBSan diagnostics |
| `tsan` | **48,308 checks** in 11 binaries, 0 failures; 0 ThreadSanitizer diagnostics |
| `uitest` | 398 passed, 0 failed (23 scripts) |
| `dbtest` | **17,999 checks** in 12 binaries, 0 failures |
| `uitest-db` | 281 passed, 0 failed (15 scripts) |
| `backuptest` | 38 UI assertions, 0 failed |
| `remotetest` | 7 passed, 0 failed |
| `cstest` | 3 passed, 0 failed |
| `llmtest` | 24 real-model checks, 0 failures |
| `uitest-real` | 35 passed, 0 failed (3 scripts) |
| `offlinetest` | 110 script passes plus 24 real-model checks, 0 failed |
| `analyze` | 0 warnings across 83 C/Objective-C files |
| `dependency-report` | every pin and container digest verified; no OSV advisories |
| `tools/test_audit_mutations.py` | 10 mutants killed, 0 survived; both unchanged controls passed |

Other baseline checks: 104 tracked Markdown files, 198 relative links, 0
broken; `git fsck` clean; the only remote branch is `main`; submodules clean at
their pins; no tracked-but-ignored files; no credentials in tracked files;
Compose services pinned by digest with `no-new-privileges:true`; every GitHub
Action pinned by commit SHA; 0 `TODO`/`FIXME` markers in `src/`; all 40
`tools/*.py` and 67 `tests/*.py` compile; `pyflakes` reported 2 findings (below).

## Findings and remediations

### F1 — the vector fault sweep was nondeterministic on the hosted runner (P2, fixed)

Hosted CI run `42693799204` for `278259c`, the push that REDTEAM-017 said still
needed hosted confirmation, passed its analyzer and TSan jobs but failed both the
unit and ASan/UBSan jobs with exactly two assertions:

```
FAIL tests/test_memory_vectors.c:451: !ok && why[0]
FAIL tests/test_memory_vectors.c:452: !count && !memcmp(out, sentinel, sizeof(out))
```

Nothing recorded that failure; the next commit added a diagnostic print to the
sweep but its notes say CI was out of scope. Rerunning the failed jobs of that
same run (attempt 2, identical commit and runner image) **passed all four jobs**,
so the failure is nondeterministic. Locally, 40 serial runs of
`build/test_memory_vectors_faults` passed 40 times (`build/audit002-vectors-faults-run.log`),
so plain reruns cannot detect it.

**Mechanism.** The sweep measures one control call's allocation count and then
fails each position in turn. The HTTP decoder grew its whole-body buffer by
doubling on every delivered piece, so the number of `realloc` calls depended on
how the kernel segmented the response: a body that arrives whole doubles once,
the same body arriving in small pieces doubles once per threshold crossing. A
sweep call whose response arrived in fewer pieces than the control's therefore
made fewer allocations, never reached the injected position, and succeeded.

**Reproduction.** A generated variant of the test (tracked sources untouched)
sets a 1 KiB `SO_SNDBUF` on every other accepted stub connection so the kernel
delivers those responses in small pieces, with no sleeps. On the baseline tree
it fails **3 of 3 runs** with the hosted assertion pair and the diagnostic
`operation=5 position=1606 control_calls=1607 actual_calls=1606 ok=1`
(`build/audit002-segment/d-*.log`).

**Remediation.** `http_decoder_feed` now sizes a Content-Length body once for
declared lengths under 1 MiB (chunked, streamed-line and larger bodies grow as
they arrive), so the allocation sequence of ordinary responses no longer depends
on delivery. The first version of this fix reserved the declared length up to
64 MiB before any bytes arrived; [REDTEAM-019](REDTEAM-019-cycle-tooling.md)
measured a 128 MiB reservation for a one-byte body behind a lying header and
bounded it. A new case
in the same binary feeds one 5,000-byte response whole, in 1 KiB pieces and byte
by byte and requires equal allocation counts. Against the old decoder it fails
with `whole=2 1KiB=5 bytewise=7`; against the new one it passes. The segmented
variant rebuilt on the fixed decoder passes **3 of 3 runs with 0 divergences**
(`build/audit002-segment/d-fixed-*.log`). The hosted rerun and the two attempts'
job lists are in `build/audit002-hosted-278259c-attempt{1,2}.*`; a hosted run
of the fix itself follows the next push and is not claimed here.

### F2 — the vector test indexed −1 after an earlier failure (P3, fixed)

Under a delayed-delivery variant, UBSan reported
`index 18446744073709551615 out of bounds for type 'cJSON *[8]'` at the test's
own `stub.points[--stub.count]`: once an upsert had already failed there was no
point to remove. The removal is now preceded by `CHECK(stub.count > 0)` and
skipped when empty, so a first failure no longer cascades into undefined
behavior in the fixture.

### F3 — one run consumes most of the loopback ephemeral port range (P3, open)

Measured after a single normal run of `test_memory_vectors_faults` (0.88 s):
**11,692 sockets in `TIME_WAIT`**, against an ephemeral range of 16,384 ports
(`net.inet.ip.portrange` 49152–65535) and `net.inet.tcp.msl` 15,000 ms. In that
normal run 11,686 of them had the stub's listening port as their local port
(`build/audit002-timewait.txt`): the stub closed first, so the entries sit on
the server side and the client's ephemeral ports were not consumed. Which side
holds `TIME_WAIT` is a race between the stub's `close()` after `send` and the
client's close after the Content-Length body. A variant that paused 300 µs
between segments flipped that race and then failed
later runs within the 30 s window with `connect: Can't assign requested address`,
plus `malformed HTTP response` results whose cause was not isolated
(`build/audit002-segment/c-*.log`, `iso-*.log`). This did not produce the hosted
signature and no hosted failure of this shape has been seen. Recorded as open:
the sweep should reuse connections or make fewer of them before the suite grows,
and the decoder's "malformed" label also covers an injected allocation failure,
which makes such logs harder to read.

### F4 — hosted-CI outcomes were not carried back into the record (P3, fixed)

REDTEAM-017 ended with hosted analyzer confirmation pending. The analyzer did
pass on `278259c`, closing R6, while that run's unit and sanitizer jobs failed
(F1); none of it was written down. Separately, two CI runs on `main` on
2026-11-08 (`d20b031`, `7c99018`) were **cancelled** by `cancel-in-progress`
when the next push arrived; `f75a977` superseded both within three minutes and
passed, but under the direct-push workflow a cancelled run leaves a `main`
commit with no verdict. REDTEAM-017 now records the hosted result, and
`ci.yml` cancels superseded runs only on non-`main` refs
(`cancel-in-progress: ${{ github.ref != 'refs/heads/main' }}`). The workflow
files parse; the hosted effect is observable only after the next push.

### F5 — documentation drift (P3, fixed)

- `TESTING.md` did not list the seven Python tests added to `make test` by
  `6c243d5`, nor the vector fault sweep itself; both are now described.
- `tests/ui/store/19_manuscript_prepare.scribo` and
  `20_manuscript_ready_prepare.scribo` (added in `1bfd022`) were referenced by
  no Makefile target, document or tool; MEMORY-022 cites only their evidence
  directories. They are now named under the manual manuscript runner.
- `TECHNICAL-DEBT.md` carried the `af57a32` baseline. Re-measured per its own
  rule: `src/main.c` is unchanged at 4,533 lines, now **12.1% of 37,573**
  C/header lines (the denominator grew by 292). The accepted-debt disposition is
  unchanged.

### F6 — Python lint (P3, fixed)

`pyflakes` over `tools/` and `tests/`: an unused `import os` in
`tools/test_recall_mutations.py` and a dead `rows = []` in
`tools/model_check/lfm3_check.py`. Both removed; `pyflakes` now reports 0.

### Reviewed without change

- The ten diagnostic tools added in `6c243d5` were read in full. Each writes only
  into a fresh output directory (`exist_ok=False`), verifies the pinned model
  hash and the live slot size before interpreting anything, refuses ambiguous
  instrumentation anchors, and opens capture files with
  `O_CREAT|O_EXCL|O_NOFOLLOW` under a directory that must be owned and `0700`.
  None modifies tracked sources, production prompts, selection or models.
- `tools/memory_manifold_cases.py` is referenced only by the journal entry that
  cites its output; it is historical diagnostic tooling, left in place.
- The branch ruleset on `main` is active with `deletion`, `non_fast_forward`
  and `required_linear_history`, matching the register's description.

## Remediated-tree verification

The remediated tree (decoder fix, test guard and regression, documentation and
CI changes) was rerun through the same 17 targets. Every target exited 0, with 0
test failures and 0 compiler, analyzer or sanitizer diagnostics, in a measured
**760.1 s** of sequential wall time (`build/audit002-after-results.json`).

| Target | Result |
|---|---|
| `all` | build passed |
| `analyzetest` | 8 Python gate tests passed |
| `test` | **95,591 C checks** and 59 Python tests, 0 failures; includes the new `decoder allocations independent of delivery` case |
| `sanitize` | 95,591 C checks and 59 Python tests, 0 failures; 0 ASan/UBSan diagnostics |
| `tsan` | **48,300 checks**, 0 failures; 0 ThreadSanitizer diagnostics |
| `uitest` | 398 passed, 0 failed |
| `dbtest` | **18,000 checks**, 0 failures |
| `uitest-db` | 281 passed, 0 failed |
| `backuptest` | 38 UI assertions, 0 failed |
| `remotetest` | 7 passed, 0 failed |
| `cstest` | 3 passed, 0 failed |
| `llmtest` | 24 real-model checks, 0 failures |
| `uitest-real` | 35 passed, 0 failed |
| `offlinetest` | 110 script passes plus 24 real-model checks, 0 failed |
| `analyze` | 0 warnings across 83 files |
| `dependency-report` | pins and digests verified; no OSV advisories |
| `tools/test_audit_mutations.py` | 10 killed, 0 survived; both controls passed |

The unit and TSan totals are a few checks lower than the baseline (95,597 and
48,308) because the fault sweeps now find fewer allocation positions per call:
the decoder makes one body allocation instead of several. `dbtest` gained the
one guard check from F2. The link check over 105 tracked Markdown files finds
203 relative links and 0 broken; `pyflakes` reports 0 findings.

This report records local verification. The hosted effect of the `ci.yml`
change and a hosted run of the decoder fix follow the next push and are not
claimed here.

## Evidence

- `build/audit002-results.json`, `build/audit002-<target>.log` (baseline matrix)
- `build/audit002-after-results.json`, `build/audit002-after-<target>.log`
- `build/audit002-clean-build.log` (clean rebuild, warning count)
- `build/audit002-mutations.log`, `build/audit002-after-mutations.log`
- `build/audit002-hosted-278259c-attempt1.log/.json`, `-attempt2.json`
- `build/audit002-vectors-faults-run.log` (40 serial baseline runs)
- `build/audit002-segment/` — generated test variants A–D and the instrumented
  client copy, their run logs, `before-fix.log` and `after-fix.log`
