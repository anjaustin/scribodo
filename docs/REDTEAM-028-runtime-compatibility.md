# REDTEAM-028 — C03 runtime compatibility and evidence boundaries

**Started:** 2026-11-27. **Parent:** `4d380a4`.
**Status:** R1–R6 remediations verified; R7 promotion guard retained. Checkpoint commit/push authorized.
**Authorization:** end-to-end red-team, remediate/verify/iterate, then commit and push.
**Scope:** the C03 compatibility checkpoint and isolated upstream candidate; no author data or database
migration, no new model precision policy, no automatic production pin promotion.

## Findings

| ID | Severity / boundary | Finding | Remediation / verification |
|---|---|---|---|
| R1 | P1, acceptance oracle | Matching empty runtime/reference collections could pass. Truncated long distributions were silently skipped by `zip`. | Exact 4/10 case counts, positions and full finite vocabulary now required. Red/green regression and fresh full model reruns pass. |
| R2 | P1, artifact/precision/provenance | Main comparator ignored identity failure, wrong GGUF hash, changed flags/reference dtypes and mixed script provenance; same binary could be labeled old/new. | Compare exact precision/configuration, shared provenance and pinned revisions, identity result/hash, distinct binary hashes and expected version identifiers. Independent postflight checks actual binary hashes. |
| R3 | P1, raw-result oracle | Stored `identical` booleans could hide actual template/token mismatches; repeated cases met count checks. | Recompute equality from raw values; require unique identities and the exact ordered fixture list. Preserve original evidence. |
| R4 | P2, reproducibility/bounds | Fixed scratch/output paths plus shell redirection could truncate old driver logs before refusing an existing result. Source capture could overwrite its retrieval manifest and read an unbounded response. | Explicit new output paths, exclusive run-directory admission before logs, per-stage exit ledger, explicit fixed model environment. Source capture refuses existing directories/files before network and caps each response at 16 MiB before publication. |
| R5 | P2, lifecycle | Readiness checked a 180-second deadline but an individual health request could block for 240 seconds. Default SIGTERM bypassed Python cleanup. | One-second readiness requests; SIGTERM unwinds through owned-child shutdown. Startup-failure unit test verifies bounded request, child wait and key-directory removal. Both real adapters pass owned-server death/restart and socket cleanup. |
| R6 | P2, archival | Fixed archive paths, sidecar collisions checked too late, recursive symlink following, whole-file hashing and hardcoded scratch harmed reliable archival. | Explicit admitted run/build/name paths; preflight all receipt paths; refuse tree symlinks; stream file hashes; retain read-back verification. Collision/symlink tests added. |
| R7 | P1, promotion claim | First-pass one-slot distribution/template checks do not establish production adapter, other-model-role, context/resource or independent audit acceptance. | Keep candidate unpromoted; add available legitimate model/adapter gates and state remaining adoption limits explicitly. Never relabel unknown coverage as green. |

## Red/green evidence

`python3 tests/test_c03_acceptance.py` initially exits **1**, **15 tests / 13 failures**:
all failures are false-acceptance assertions, not timeouts/import/compile failures.
The complete synthetic fixture passes; missing-reference-NaN refusal was already effective.
The five original toy distribution tests are retained. The expanded suite is integrated into `make test`.
Exact commands, green results and complete model/gate outcomes are recorded in the composition journal.

After remediation, **27/27** synthetic regression checks pass. The final added response-size check
was also run red first (one assertion failure, exit **1**), then green with the response cap.
A fresh full model run (`parity-02`)
passes artifact/F32-reference/baseline/candidate/comparison stages, all exits **0**, with independent
postflight **0** and the same distributions/continuations as the preserved first run.

The normalization diagnostic is not hidden or “fixed” by rewriting model observations. Using a
triangle-inequality bound on the recorded F32 residuals, even if the reference were renormalized,
candidate worst gaps could be at most **0.022689 short / 0.083732 long**, still below the original
0.05/0.12 gates. This sensitivity check does not establish the residual's numerical cause.

## Extended end-to-end verification

The serial `run-redteam-gates.sh` working-tree driver exits **0** with all **10** stage exits **0**:
oracle, live baseline, live candidate, roles baseline, roles candidate, full test, analyzer, ASan/UBSan,
TSan and whitespace. Analyzer reports zero warnings across 113 sources. Full test/sanitizer include the
evidence registry and public-domain observer yardstick without skip overrides.

- Existing real C adapter suite: **24 checks / zero failures** on each runtime; private authentication,
  exact 133-token source budget and amber oracle, streaming, dense CJK cutoff, neutralization,
  owned-server death/restart and socket cleanup.
- Valid production-derived JSON-schema requests: **four source-exact extractions per runtime**, including
  repeated slot 0 with cache reuse and switching to slot 1, with identical request bytes.
- Cached, hash-admitted Qwen embedding role: all four 1,024-component vectors are finite and **component-
  identical between runtimes**; the identical source ranks first.
- Cached, hash-admitted BF16 Qwen reranker role: the three synthetic pairs retain the same scores
  **1/0/0** and answer-bearing first place, under the existing top-20 yes/no scoring convention.
- All 22 primary-source response hashes/sizes revalidated. Credential-pattern scan of 138 explicitly
  admitted source/evidence files finds zero matches; this supplements admission, not a general DLP proof.
- No model server remains. Only Apple/system dynamic libraries were admitted in the candidate build.

Raw requests/replies/logs, stage exit ledgers, platform/toolchain metadata and source/build snapshots
are preserved by the C03 evidence archives. The final committed-snapshot rerun is a push prerequisite,
not silently inferred from working-tree success.

## Test boundary

Malformed-input reproducer remains uncompiled/unexecuted. Public upstream source/patch review and ordinary
upstream suites are allowed; no alternative reproducer or safety-layer bypass is introduced.
Tests use synthetic observations or cached admitted official model artifacts. No author project is used.

This is a same-agent adversarial review, not an independent audit or a claim of universal security.
Only findings with recorded successful remediation checks can be closed. A tooling checkpoint may be
committed while candidate adoption is explicitly blocked; committing it must not change the product pin.

The runtime pin is **not changed in this checkpoint**. R7 is safely contained, not evidence that all
adoption work is done. Larger held-out production captures, whole-system memory/context/checkpoint/
contention behavior, clean-install build-recipe/licensing closure, affected real UI/retrieval/observer
acceptance and independent review remain prerequisites for a separate production pin decision.
Single warm startup/RSS observations are descriptive only. DB/crash/UI suites were not rerun for this
dev-only tooling checkpoint; no production store/UI code changed. No universal “100% pristine” claim.
