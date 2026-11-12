# The register: the experiment environments, declared

**Date:** 2026-11-12. **Tree:** `de82f6d` plus the changes recorded below.

The register's entry "unarchived evidence and undeclared experiment
environments" (AUDIT-003 F6) had two halves. The environment half: two
virtualenvs under `build/` (`venv-coref`, 1.1 GB; `venv-causal`, 797 MB)
with their versions in journal prose only, removed by `make distclean`.

`tools/envs/venv-coref.lock` (79 packages) and `tools/envs/venv-causal.lock`
(35) were written from the live environments' installed distributions
(`importlib.metadata`), one `name==version` per line; the spaCy model
`en_core_web_sm` 3.8.0, which is not on PyPI, is pinned by its release wheel
URL. Both environments were made with uv 0.13.6 on Homebrew Python 3.12.15
(`pyvenv.cfg`), so `make envs` recreates each the same way (`uv venv`, then
`uv pip install -r`) and marks it with a copy of its lock; `make tools` builds
the two tool binaries. `make envs` was run against the existing environments: uv checked the 79 and
35 pinned packages in milliseconds and installed nothing, which is what a
correct lock does on the environment it describes; a fresh recreation (which
downloads torch) was not run here. `make test` on this tree: 95,917 C checks
and 167 Python tests, 0 failures, exit 0 (`build/envs/gates-test.log`); the
other gates compile nothing this change touches.

The archive half: the `build/` evidence of the 2026-11-11 and -12 steps
(window, flags, receipt, margin, crash, relation, atomic, environments, the
third novel's import) is `scribodo-evidence-2026-11-12-p0.tar.gz` (656 files,
3.8 MB) with its manifest and `SHA256SUMS` entry, recorded in EVIDENCE.
