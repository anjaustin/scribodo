# C03 — runtime security and dependency triage

Started 2026-11-26 at `4d380a4` with a clean worktree.
Canonical status/commands: [composition001-progress](../../composition001-progress.md).

## Current questions

- D1: Four pinned-commit OSV records now match (two newer than AUDIT-005). What exact inputs reach the affected GGUF loader
  and sampler paths? Which reviewed fixes descend from / can be applied to the current runtime?
- D2: Is the pinned MariaDB manifest still available? Did the tag move because of a platform child,
  rebuilt base/layers, version change or another upstream action? Compare immutable manifest/config
  evidence before changing anything; tag movement alone is not compromise.
- For an eventual repair, what source/kernel, artifact, license, template/tokenizer, reference-distribution
  and real-model checks are mandatory? Mechanical tests alone cannot clear a runtime change.

## Boundaries

No dependency pin, model configuration, precision policy or schema change yet. No live exploit or
untrusted model execution. Network queries are read-only primary-source/registry checks.
No vulnerabilities are declared unreachable until their actual input paths and limits are evidenced.

## Compatibility checkpoint

The offline pinned reference environment and serial benign MODEL-001 comparison completed successfully.
The original `run-compatibility.sh` snapshot in the archive is the first run's exact command manifest;
the current hardened runner requires explicit new scratch/output paths.
`compatibility_probe.py` reuses the existing reference code and private Unix HTTP client without changing
legacy scripts/output. It does not execute `grammar_rejection.cpp`, which remains uncompiled/unexecuted.

Raw run directory:
`/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo-c03-runtime.D8CKUp/parity-01`.
Summary and precision inventory: `parity-compare.log`, `parity-postflight.json`, `parity-kernel-review.json`.
Full source/link, production-request, resource, adversarial and adoption limits remain in
`docs/COMPOSITION-001-runtime-security.md`; the candidate is not promoted.

`verify_parity.py` independently checks the stored observations, without a model process. Its first
attempt wrongly assumed vocabulary size 128256 (actual pinned config: 128000); the next introduced
a stricter, unapproved normalization diagnostic. Both exit **1** and logs are retained. The corrected
postflight exits **0**, reporting normalization residuals without modifying observations or MODEL-001
limits. `archive_parity.py` preserves raw bytes/configuration/source snapshots without models or secrets.

Archival completed **0**: 201 files, 14,370,535 compressed bytes, every member rehashed on read-back.
See `parity-archive.json` and `docs/EVIDENCE.md`; the immutable archive does not include subsequent
receipt/documentation edits. Do not overwrite the completed run/archive. The hardened archive creator
now requires explicit new run/build/name paths and preflights archive/manifest/checksum collisions.

## REDTEAM-028

The author authorized remediation/verification and a bounded checkpoint commit/push on 2026-11-27.
`tests/test_c03_acceptance.py` initially exposed 13 false-acceptance assertions in 15 tests; the expanded
27-case suite and the unchanged five toy tests now pass. `parity-02` and its independent postflight
pass all original quality/identity limits under the repaired gate, with explicit exit/version records.
`run-redteam-gates.sh` serializes actual live adapter tests, benign schema/other-role smoke and full
mechanical gates. `role_smoke.py` freezes synthetic inputs/criteria before execution; its timing/RSS
measurements are descriptive, not a new budget or whole-system acceptance.

The full working-tree matrix passes, all ten exits 0, including both live C adapters (24 checks each),
four source-exact schema/cache/slot extractions per runtime, identical synthetic embedding vectors/
reranker scores, full test/analyzer/ASan/UBSan/TSan. R1–R6 repairs are verified; R7 remains contained by
the explicit no-promotion boundary. `redteam-archive.json` records the 264-file rehashed raw archive.

`verify-committed-and-push.sh` requires clean exact HEAD, repeats parity and the complete matrix, archives
the committed observations, checks unchanged clean HEAD again, then pushes and verifies the remote ref.
It is authorized for this bounded tooling checkpoint, not a product runtime pin or DB digest change.
