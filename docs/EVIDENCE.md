# Local evidence archive

Dated reports under `docs/` cite run directories and logs by their original
`build/` paths, for example `build/mn-z0h05o1h` in MEMORY-011 or
`build/memory-quality` in MEMORY-009. Those paths were working locations on the
development Mac, not tracked files. Nothing cited is deleted; it is archived.

| Archive | Contents | Where |
|---|---|---|
| `scribodo-evidence-2026-11-07.tar.gz` | Every `build/` directory and log cited by a doc as of 2026-11-07: the native continuity runs (`mn-*`, `mo-*`), the causal parity and bedrock probe runs (`mc-*`, `mb-*`), `memory-quality`, `memory-mutations`, `remediation-mutations`, `redteam2-fresh`, `hangs`, and the `memory-bedrock-*`, `memory-redteam-*`, `window-audit-*` and `async-history-retry-acceptance` logs | `~/Library/Application Support/Scribodo/evidence/` with `SHA256SUMS` and a `manifest-2026-11-07.txt` listing the top-level entries |

| `scribodo-evidence-2026-11-08.tar.gz` | The S2 and S3 reach runs (`build/memory-reach/runs/*`, including seeds 1 to 4), the four native model-side runs (`build/mn-*`: the sleep-interrupted run, the complete 144-answer run, the two near-miss runs), the stall replay (`build/stall-replay`), the hang probe (`build/hang-probe`) and the lexical bench reports (`build/lexical-bench`) | same folder, `SHA256SUMS` and `manifest-2026-11-08.txt` |

| `scribodo-evidence-2026-11-08b.tar.gz` | The canon-aware chat run (`build/mn-ld40aqkd`, MEMORY-020) and its episode fixture, the two document-reach runs rescored in MEMORY-017 and the two runs after the unit-cut change (`build/memory-reach/runs/scribodo_test_reach_{98605,98703,5807,5959}_*`), and `build/hangs` including the sleep-interrupted `test_llm` samples | same folder, `SHA256SUMS` and `manifest-2026-11-08b.txt` |

| `scribodo-evidence-2026-11-08c.tar.gz` | The real-manuscript work (MEMORY-021): `build/manuscripts/pride-and-prejudice` with the Gutenberg source and checksum, the imported chapters and parts, and under `runs/` every application run log, database sample, probe script and screenshot; the reach run over the novel (`build/memory-reach/runs/scribodo_test_reach_15740_*`) | same folder, `SHA256SUMS` and `manifest-2026-11-08c.txt` |

| `scribodo-evidence-2026-11-08d.tar.gz` | The red-team of the real-manuscript changes (REDTEAM-016): `build/manuscripts/pride-and-prejudice/runs/redteam016` (the explorer-scroll probe against the baseline and the fixed app, the baseline import test run) and the suite logs `build/make-{test,uitest,dbtest,tsan}-redteam016.log` | same folder, `SHA256SUMS` and `manifest-2026-11-08d.txt` |

The 2026-11-09 repository audit and follow-up red-team evidence is in
`scribodo-evidence-2026-11-09-audit.tar.gz` in the same folder, with
`manifest-2026-11-09-audit.txt` and an entry in `SHA256SUMS`. It contains 108 files:
the `build/audit-*.log`/`.json` and `build/redteam017-*.log`/`.json` run records,
the generated mutation sources/logs/results under `build/audit-mutations/`, and
the clean shallow checkout's Connector/C and analyzer logs. It excludes compiled
binaries and the shallow checkout itself. Archive SHA-256:
`796386b993c464e353a5a650c99f3c6f8e918d5b2d3a97e7622c075c810715f3`.

The hosted-CI iteration is preserved separately in
`scribodo-evidence-2026-11-09-audit-ci.tar.gz` (87 files), with
`manifest-2026-11-09-audit-ci.txt` and its `SHA256SUMS` entry. It includes the
initial hosted analyzer failure, trusted-acceptance result, corrected-tree
16-target rerun, refreshed mutation controls and fresh Connector/C build logs.
It precedes the corrective push's hosted rerun. Archive SHA-256:
`f4cdef24571a58d0d0ff1a1b71ef7ffe582e57c0345e2c372be6461e976828d5`.

The AUDIT-002 evidence is in `scribodo-evidence-2026-11-09-audit002.tar.gz` in
the same folder, with `manifest-2026-11-09-audit002.txt` and an entry in
`SHA256SUMS`. It contains 80 files: the baseline and remediated 17-target matrix
logs and results (`build/audit002-*.log`, `build/audit002-after-*.log`, both
`results.json` files), the clean-rebuild log, the hosted-CI attempt records for
`278259c`, the 40-run baseline log, the `TIME_WAIT` snapshot, and under
`build/audit002-segment/` the generated test variants, the instrumented client
copy and every run log including `before-fix.log` and `after-fix.log`. Compiled
binaries and the `build/audit002-clean` build tree are excluded. Archive SHA-256:
`fb20ec39da5ef058fd8545e6cfb6fa46d5ad1b23803d5de112d1c4f260cc89e3`.

The answer-cycle and REDTEAM-019 evidence is in `scribodo-evidence-2026-11-09-cycle.tar.gz`
in the same folder, with `manifest-2026-11-09-cycle.txt` and an entry in
`SHA256SUMS`. It contains 154 files: the income isolation run (`build/ai1`), the
strict-CPU arm (`build/pq2`), the boundary live capture and review
(`build/answer-boundary-capture`), the boundary run's exported messages, units,
work copy and logs (`build/product-memory-use/boundary-1`, frozen binary
excluded), the fake-backend dry run, the quotation-binding and fixture-binding
outputs, the REDTEAM-019 17-target matrix logs and results, and the generated
mutation sources and logs under `build/redteam019-mutations/` (binaries excluded).
Archive SHA-256: `b3e1f0b5ab008d5ae580b8f1c2d979dbecfdf73f6b852d995f966fffaeb5f575`.

To consult a cited path, extract the archive into an empty folder and read the
path relative to it. Stale `llama.sock` files inside a few run directories were
not archivable and are not evidence.

Rules:

- A doc keeps citing the original `build/` path; this file maps it to the archive.
- New evidence stays in `build/` until the next archive pass, which adds a new
  dated tarball here rather than rewriting an existing one.
- The archive folder is outside the repository and is never committed. It is
  covered by ordinary machine backups, not by project backups.

The answer cycle 3 and AUDIT-003 evidence is in
`scribodo-evidence-2026-11-11-audit003.tar.gz` in the same folder, with
`manifest-2026-11-11-audit003.txt` and an entry in `SHA256SUMS`. It contains
774 files: the offline coreference arms (`build/cr1`, `build/cr2`), the seven
pass runs of the verified pipeline (`build/answer-live-pipeline` to
`-pipeline7`), the three coreference treatment runs and their control
(`build/answer-live-coref`, `-coref2`, `-coref3`), the pipeline review outputs
(`build/answer-pipe-capture`), the gate replay sheets (`build/gate-replay-*.json`),
the REDTEAM-021 mutation sources and logs (`build/redteam021-mutations`), and
the AUDIT-003 matrix logs, results and the remediated `dbtest` log
(`build/audit003`). Instrumented binaries and objects are excluded (31 files).
Archive SHA-256:
`25668b343d4e8aac3ca6c30a49dcd6b46a1c6a7994e4de62dc0b8883acb8333e`.

The second manuscript's evidence (answer cycle 4, `journal/answer-cycle4_mars.md`)
is in `scribodo-evidence-2026-11-11-mars.tar.gz` in the same folder, with
`manifest-2026-11-11-mars.txt` and an entry in `SHA256SUMS`. It contains 630
files: the imported novel with its Gutenberg source and checksum
(`build/manuscripts/a-princess-of-mars`), the two reach runs at the same tree
(`build/memory-reach/runs/scribodo_test_reach_63447_*` for the second novel and
`..._63867_*` for the first novel's control), the two live runs' exported
messages, units, manifests, logs and work copies (`build/product-memory-use/mars-pipe0`
and `mars-pipe1`), the live-capture requests, streams and outcomes, the gate
replays before and after the capitalisation change, the gate logs and the
review sheets (`build/answer-pipe-capture/review-pipeline-mars{0,1}.json`), and
the instrumented builds' generated sources and build logs. Frozen binaries and
objects are excluded (6 files). Archive SHA-256:
`d2a305e62be15201e316c6c1d32e866a2d34d1f7c42277d49e9569e2b2ccbed8`.
