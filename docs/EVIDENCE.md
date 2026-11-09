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

To consult a cited path, extract the archive into an empty folder and read the
path relative to it. Stale `llama.sock` files inside a few run directories were
not archivable and are not evidence.

Rules:

- A doc keeps citing the original `build/` path; this file maps it to the archive.
- New evidence stays in `build/` until the next archive pass, which adds a new
  dated tarball here rather than rewriting an existing one.
- The archive folder is outside the repository and is never committed. It is
  covered by ordinary machine backups, not by project backups.
