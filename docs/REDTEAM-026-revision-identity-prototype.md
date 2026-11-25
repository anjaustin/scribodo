# REDTEAM-026 — C02 prototype and evidence

**Date:** 2026-11-25. **Parent production checkpoint:** `50286c2`.
**Scope:** the uncommitted C02 design, native key experiment, public-API red contract probe and their evidence.
**Status:** R1–R4 repaired and verified; repository gates passed. No production F2 repair is claimed.

## Findings

| ID | Severity/scope | Finding | Remediation |
|---|---|---|---|
| R1 | P1, fixture confinement | `dirs` accepted any `EEXIST` object and followed symlinked directories, including a planted intermediate directory. The helper could write outside its intended synthetic subtree. Empty paths also lacked a valid input contract. | Walk absolute paths with directory descriptors, `mkdirat` and `openat(O_DIRECTORY | O_NOFOLLOW)`. Reject empty/relative/null paths and `.`/`..` components. Snapshot reads/writes use the same descriptor traversal and no-follow/exclusive opens. |
| R2 | P2, output contract | A failed `fixture_path` returned a truncated but nonempty output, unlike the strict codec functions. Reusing that output could point later fixture work at an unintended path. | Clear failed output, reject unusable inputs, and test overflow explicitly. |
| R3 | P2, test oracle | The rename/archive checks used only `access(F_OK)`. A moved file containing wrong bytes could pass. This was insufficient evidence for retaining immutable snapshots. | Require a regular file, exact length and exact bytes. Add equal-length tampering and symlink controls; neither may pass. |
| R4 | P2, red-test validity | The contract probe continued after some failed setup and counted setup/control problems alongside expected product defects. A broken fixture could therefore look like a valid “F2 still red” run. Some fixture joins also ignored truncation. | Checked joins; stop each case on setup failure; distinguish setup/control exit **2** from contract-failure exit **1**; require the ordinary and legacy-short controls to work. A generated fixture-failure control verifies that distinction. |

These are development-harness defects, not new claims about shipped product behavior. Their original
source and red execution are retained under `journal/composition001/c02/redteam/`. The fact that the old
prototype passed 71,648 assertions did not cover these boundaries; assertion count is not an assurance score.

## Verification and iteration

- New confinement/overflow assertions failed against the original key prototype: **71,660 checks,
  five assertion failures**, process exit **1**, with no sanitizer finding. Original and red source are
  retained as `key-probe-before.c` and `key-probe-red.c`; neither is linked into the product.
- The repaired final native key control passes **101,676 checks** under ASan/UBSan, including 10,000
  deterministically generated arbitrary-byte names/corrupted-key cases. Accepted decoded keys must
  re-encode canonically; invalid decoded outputs must be empty.
- All **five** deliberately weakened variants fail by assertions: following directory symlinks, exposing
  truncated paths, ignoring snapshot bytes, ignoring owner bytes, and accepting noncanonical uppercase hex.
  Compiler errors, sanitizer findings, crashes and timeouts are not accepted as killed mutants.
- The second pass also rejects a FIFO snapshot without blocking (`O_NONBLOCK` followed by a regular-file
  check). Both standalone current probes pass Clang analysis with no diagnostics; archived pre-fix sources
  are retained evidence, not the claimed repaired control.
- The updated public-API F2 contract still reports **52 checks, 12 failures, zero setup/control errors**.
  That is the expected unresolved product defect, not a green remediation result.
- A fixture-creation failure injected into a generated copy reports setup/control exit **2**, separately
  from the real contract's expected exit 1. The harness no longer passes off missing fixtures as F2 evidence.
- Exact commands, exit statuses, final repository gates and commit/push disposition are in the
  [progress journal](../journal/composition001-progress.md).
- Final `make test` and `make sanitize` passed serially (both exit **0**); product analyzer exited **0**
  with no warnings across 113 sources. No production source changes are part of this checkpoint.

## Design review and next boundary

The reversible key remains a **representation candidate**, not a production selection. It removes a
digest-owner metadata publication boundary, but its longer path, logical enumeration, archive generations,
dated rename/source references and legacy ambiguity policy still require integration tests. The SHA-256
alternative's owner predicate is tested; its durable owner-publication protocol is not implemented.
No source of truth is inferred from a hash, current document text or import ordering.

The design correctly calls out that rename/set-aside and SQL import reconstruct directory identity outside
`doc_dir`. That remains the decisive reason not to hot-swap just the writer. The ordinary legacy,
read-only, name-reuse and deleted-document cases must be preserved in the eventual repair.

This review does not certify “pristine” product-wide behavior or close AUDIT-005 F2/D1/D2. It closes
the identified prototype/evidence findings after the final verification pass. No author data, model request,
dependency pin, DB schema or production format is changed in this checkpoint.
