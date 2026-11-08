# Local evidence archive

Dated reports under `docs/` cite run directories and logs by their original
`build/` paths, for example `build/mn-z0h05o1h` in MEMORY-011 or
`build/memory-quality` in MEMORY-009. Those paths were working locations on the
development Mac, not tracked files. Nothing cited is deleted; it is archived.

| Archive | Contents | Where |
|---|---|---|
| `scribodo-evidence-2026-11-07.tar.gz` | Every `build/` directory and log cited by a doc as of 2026-11-07: the native continuity runs (`mn-*`, `mo-*`), the causal parity and bedrock probe runs (`mc-*`, `mb-*`), `memory-quality`, `memory-mutations`, `remediation-mutations`, `redteam2-fresh`, `hangs`, and the `memory-bedrock-*`, `memory-redteam-*`, `window-audit-*` and `async-history-retry-acceptance` logs | `~/Library/Application Support/Scribodo/evidence/` with `SHA256SUMS` and a `manifest-2026-11-07.txt` listing the top-level entries |

To consult a cited path, extract the archive into an empty folder and read the
path relative to it. Stale `llama.sock` files inside a few run directories were
not archivable and are not evidence.

Rules:

- A doc keeps citing the original `build/` path; this file maps it to the archive.
- New evidence stays in `build/` until the next archive pass, which adds a new
  dated tarball here rather than rewriting an existing one.
- The archive folder is outside the repository and is never committed. It is
  covered by ordinary machine backups, not by project backups.
