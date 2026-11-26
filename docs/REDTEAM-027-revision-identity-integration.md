# REDTEAM-027 — revision identity and namespace publication

**Date:** 2026-11-25. **Parent checkpoint:** `1c1ab15`.
**Scope:** C02 production integration, synthetic fixtures only.
**Status:** R1–R7, full mechanical gates and final scripted UI acceptance verified.
C02/F2 is ready for its bounded checkpoint; the author authorized commit/push on 2026-11-26.

## Findings and disposition

| ID | Scope | Finding | Disposition |
|---|---|---|---|
| R1 | P1, interrupted publication | Moving a manuscript/history then appending a rename event has multiple externally visible states. Rollback cannot retract an uncertain append. | Replaced with a replayable receipt retaining full names, inode/device guards, operation ID and time. Matching log events are re-fsynced, not duplicated. Process-death, allocation-failure and full acceptance gates pass at the boundaries below. |
| R2 | P1, recovery entry points | A History-only recovery hook lets source resolution or manuscript open see an unrecovered namespace first. | Explicit recovery before file-project/document opens and source-name resolution. Failed resolution returns an unavailable result rather than the original name. Immediate first-reader receipt/inode assertions pass and fail with replay disabled. |
| R3 | P1, filesystem aliases | Equal inode/device pairs can be hardlinks, not merely case/normalization aliases. Renaming one hardlink over another can be a no-op while history/log publication claims a move. | Refuse manuscript namespace moves unless the recorded file has one link, including on replay. Dedicated refusal preserves both manuscript names and separate histories. This is a conservative refusal, not hardlink support. |
| R4 | P2, complete-name parsing | A fixed 2048-byte rename-log reader cannot represent two 255-byte basenames whose control bytes require six-byte JSON escapes. | Complete no-follow file read and dynamic line parsing; escaped-name control plus operation-ID mismatch refusal pass. No new guarantee for arbitrary manually corrupted historic logs. |
| R5 | P2, descriptor validation | Analyzer found a possible negative `dirfd()` passed to `openat()` while recognizing interrupted format-tag temporaries. | Check the descriptor before use. Original analyzer exited 2; final analyzer exited 0 with no warnings across 113 sources. |
| R6 | P2, test oracle strength | The interruption driver can retry a rename after the first reader. Its final receipt-absence assertion alone does not prove that the first reader completed recovery. | Observe the receipt before the first reader, then assert its absence and the moved manuscript inode immediately afterward, before inventory/retry. The unmodified suite passes; disabling first-reader replay fails by assertions / exit 1. |
| R7 | P1, uncertain retry result | A retry can replay a pending receipt far enough to move the manuscript, then fail at History/log publication. The rename API previously left `file_moved` false on this recovery path, so the live editor could retain the old path. | Red run: 1,145 checks / exactly one assertion failure, exit 1. Repair reports the validated matching request's actual destination inode immediately after its manuscript step, even when sync/later publication fails. Green run: 1,145 checks, zero failures; all five negative controls fail as intended. Final full/scripted UI gates pass. |

## Evidence currently available

- Original public F2 contract, rerun on final code: 52 checks, zero failures/setup errors.
- Expanded file-only recovery: initially 988 checks; strengthened suite **1,144 checks**, zero failures,
  including 48 interruption/control cases and complete-valid-receipt-plus-trailing-garbage refusal.
  Final R7 suite: **1,145 checks**, zero failures, in normal and ASan/UBSan full runs.
  The tests kill the process at receipt publication, manuscript/history movement, log publication,
  receipt removal and legacy format-tag publication; they verify inode retention, exact original text
  and dated sources for both the moving manuscript and an older destination generation.
- Allocation faults: 196 sites, 392 one-shot/permanent cases; complete persistence suite 8,059 checks,
  zero failures. Successful replay retains exactly the two expected acknowledged operations.
- Final isolated DB suite: new seven-source identity round trip and ambiguous-old-bucket refusal passed.
  Source contents must equal immutable revision contents, and document/revision IDs must agree; mere row
  existence is not the oracle. `test_db` reports 12,883 checks, zero failures.
- New scripted UI tests cover shared long prefixes and unavailable legacy ambiguity. The existing
  read-only-history rename test now requires manuscript refusal/preservation, not an after-move warning.
  The focused rerun passes 39 assertions across the two affected scripts. The refused-rename error is
  displayed inside the modal prompt, not the status bar; the test checks the still-open prompt, no success
  status, original manuscript name and preserved histories. Retained screenshots were visually inspected.
- All five negative controls fail by assertions / exit 1: first-reader replay disabled, inode identity
  guard removed, duplicate log publication, trailing receipt garbage accepted, and hardlinks admitted.
  The unmodified control exits 0. Builds, hashes and run logs are retained in
  `journal/composition001/c02/mutation-evidence/`; no compiler error, timeout or parent crash is counted.
- Final analyzer, TSan, DB, DB interruption and DB UI gates all exit 0. The original full test/sanitizer
  attempts stop at the evidence registry because the new test binary was not listed; that registry is
  now corrected and full test/sanitizer reruns passed. File UI originally passes 522 assertions with one
  incorrect status-bar expectation; the corrected full file UI rerun passed. These passes predate R7.
  The final R7 full test/sanitizer, analyzer, TSan, isolated DB/crash and five negative controls all pass.
  Final file UI, case-sensitive-volume UI and DB UI initially could not create a window; their nonzero
  exits and representative stderr remain retained. After the author unlocked the Mac, all three reran
  successfully on final code: **524 assertions / 30 file scripts**, **3 case-sensitive-volume assertions**,
  **317 assertions / 16 DB scripts**, zero failures. Both isolated service start/stop steps also exit 0.

Commands, logs, statuses and current handoff live in
[composition001-progress](../journal/composition001-progress.md).
`journal/composition001/c02/run-final-gates.sh` runs tests, sanitizers, analyzer, TSan, DB, crash, file UI
and DB UI **serially**, then stops only its isolated services. It records every gate exit separately.
Do not launch competing database gates or shared-yardstick runs while that driver is active.

## Limits and checkpoint disposition

This is a same-agent adversarial pass, not an independent audit.
R1–R7 and the five negative controls are verified at the boundaries stated above. Final-code scripted
UI acceptance now passes as well; C02 is ready for a bounded checkpoint, not already committed or pushed.
A successful mechanical suite does not prove power-loss behavior,
hostile concurrent ancestor replacement, real IME/accessibility, or real-model quality.
No author data, model request, precision policy, dependency pin or SQL schema was changed.
