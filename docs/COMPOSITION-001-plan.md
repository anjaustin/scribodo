# COMPOSITION-001 — lightweight composition (active P0)

**Authorized:** 2026-11-25, by the author: “This is now P0… plan … track through sessions and get started.”
**Baseline:** `69b98abf1ecf86c560b435a209327175c182eab1`.
**Status:** in progress. C01 and its REDTEAM-025 follow-up are verified.
**Active slice:** C02, revision identity/legacy compatibility design and red contract probe; no real-data migration.
**C02 boundary map and design gates:** [revision identity](COMPOSITION-001-revision-identity.md).
**Session handoff and evidence:** [composition001-progress](../journal/composition001-progress.md).
**Research:** [Lightweight components for Scribodo](../reports/Lightweight%20components%20for%20Scribodo.md).
**Implementation map:** [ARCHITECTURE](ARCHITECTURE.md). **Safety findings:** [AUDIT-005](AUDIT-005-repository-audit.md).

## Goal and boundaries

Compose the lightest mature mechanisms that improve Scribodo, retiring redundant product responsibilities
when a replacement earns adoption. Keep the custom authoring workbench, not a competitor application or
general-purpose agent framework. Efficiency means the **whole system**: maintained code, dependencies,
processes, model memory, UI responsiveness, recovery and installation—not merely fewer C lines.

This is the active P0 working order. Existing GAP-001 to GAP-005 remain recorded; this work addresses their
foundations rather than declaring them closed. Do not spend sessions on unrelated features or broad cleanup.

Nonnegotiable:

- Exact originals, source identities and author-only canon/document changes; no automatic fact promotion.
- Consent, source eligibility before top-k, generation/lease fencing, bounded work and explicit failure.
- Offline macOS/Apple Silicon; existing generative BF16/F32 contract; no silent remote fallback.
- No production manuscript, database or index migration during a prototype. Use disposable public-domain or
  synthetic fixtures, isolated credentials and ports. Retain originals until independent restore succeeds.
- No permanent parallel authorities. Prototype adapters live outside the default product route.
- Preserve concluded experiments/evidence in the harness; do not delete user work or untracked prior research.

## Work ledger

Statuses: **queued → active → verified → promoted**, or **rejected/blocked** with a reason.
“Verified” names the specific tests/measurements; it does not imply all acceptance gates passed.
Only one production-changing slice is active at a time. Each slice should be independently reviewable,
with its tests and evidence; no commits or dependency pin changes without recording exactly what they contain.

| ID | Slice / deliverable | Exit gate | State |
|---|---|---|---|
| C00 | Canonical plan, P0 register pointers and next-session handoff | Discoverable from repo entry points; outstanding decisions explicit | Complete |
| C01 | Fix AUDIT-005 F1/F3: file chat failure guards and successful retry acknowledgement | Regression red/green; repeated successful retry writes nothing; no fabricated DB sequence/window; failed saves remain protected | **Verified:** REDTEAM-025, unit/analyzer/sanitizers/TSan, file UI, isolated DB/crash/DB UI; no real-model or IME/accessibility claim |
| C02 | Fix F2 revision identity collisions, with legacy compatibility | Disjoint long/sanitized names; old histories/source refs/rename/export/import preserved; ambiguous old collisions disclosed, not guessed | **Active:** consumer map/design; synthetic contract red (52 checks, 12 failures); production remediation pending |
| C03 | Runtime security/dependency triage (D1/D2) | Reviewed fixes/pin ancestry, licence inventory, strict precision/template/tokenizer and real-model gates; explain moved image tag | Queued |
| C04 | Frozen cost/behavior baseline and dev-only fuzz entry points | Reproducible workload manifest, counts, raw logs and failing negative controls; no production fuzz runtime | Queued |
| C05 | Embedded storage feasibility: upstream SQLite + FTS5 | Disposable parity prototype and concurrency/recovery results; explicit adopt/reject decision | Queued |
| C06 | Retrieval comparison: Qdrant exact, eligible-array exact, release-pinned sqlite-vec | Same sources/eligibility/fusion; no scope/reach regression; total-cost decision before ANN | Queued |
| C07 | Native text island: direct NSTextView, borrow narrow MIT bridge patterns | Real IME/accessibility/offset/anchor/undo parity inside current window; cost comparison and adopt/reject decision | Queued |
| C08 | Narrow Unicode/name-key and transport trials | Separate utf8proc-derived-key and existing-libcurl experiments; no byte/identity/cancellation regression; demonstrable simplification | Queued |
| C09 | Observer improvements without framework accumulation | Held-out literary alias/coreference ablation; source/authority invariants and quality/resource gates; fourth model only if earned | Queued |
| C10 | Promote winners, migrate safely and retire obsolete runtime paths | Full gates, offline clean-Mac install/restore, rollback evidence, updated SBOM/docs, measured net system reduction | Queued |

C03 can supersede C02 if triage establishes an exposed urgent vulnerability; record the reason.
C05/C06 share the eventual storage decision but are separate experiments. A derived SQLite vector index can be
tested while MariaDB remains authority; that temporarily adds a store and is not itself a simplification.
C07 is independent of that decision. Start it after the storage/retrieval feasibility decision, not as an
unrelated UI rewrite. C08 contains two separate steps, not a bundled dependency upgrade.

## Execution contracts

### C01 — first bounded production slice

Use the existing `test_llm` suite. Failed persistence must be visible to New/open/quit guards in file mode as
well as DB mode. Successful file acknowledgement clears a failure mark but assigns no database sequence and
does not activate a database window. A failed retry leaves marks intact; a second successful retry appends
nothing. A window with an unsequenced message remains unsaved even after a file acknowledgement.

Do not add an automatic file retry loop or claim exactly-once delivery after an uncertain write/commit. Those
need a separate durability/idempotency contract; this slice fixes the demonstrated successful-retry defect.
Use the existing callback/retry paths. Keep actual UI failures distinct from headless API checks.

### C02/C03 — protect the baseline before measuring alternatives

F2 requires an explicit identity/migration design: hashes/IDs rather than another truncation rule.
Test legacy source lookups and DB exports through the file backend; do not simply rename every existing
history directory. Retain recoverable ambiguity when the old format has already merged histories.

For D1, check actual reachability and fix ancestry, then run the model update gates in MODEL-001.
Never equate latest HEAD/release with a reviewed compatible pin. Investigate D2 before changing its digest.
No model-facing change is promoted on mechanical tests alone.

### C04 — baseline and decision card

Use existing manuscript/soak/replay tools before writing another benchmark framework. A run records tree,
hardware/OS/toolchain, source/model/library pins, all experiment arms, fixture hashes, commands, exit codes,
environment exclusions, sample counts and raw logs. Test novels must be licensed/public-domain and held-out
quality questions must not become tuning data.

Record both index/library-only cost and whole-app plus service/model cost:

- first-party production lines/build dependencies, shipped processes/artifacts and operational commands;
- cold/warm startup and build, RSS/available-memory change, on-disk size and background CPU;
- UI frame/input tail, prompt preparation, time to first token, retrieval and save/history/restore timings;
- exact-neighbor agreement, answering-source reach, faithful use and observer precision/recall separately;
- contention, cancellation/deadlines, interruption/recovery, offline setup and licensing obligations.

Carry forward agreed budgets (including the existing worst-frame rate and preparation cap).
Before each experiment, fill its decision card: **hypothesis, bytes/code/processes removed, new obligations,
controlled variables, corpus, metrics, pass/fail thresholds, safety oracle, rollback and cost ceiling**.
Proposed new numeric budgets must be identified as proposed and approved before promotion, not selected
after seeing a favorable result. No fabricated baseline or universal percentage improvement target.

Add at most a few standalone libFuzzer targets around pure parsers/span/receipt code, with capped allocations
and seeded deterministic oracles. Keep them dev-only; don't introduce overlapping test frameworks.

### C05/C06 — embedded storage and exact retrieval

Pin reviewed upstream SQLite with FTS5, not an assumed system version; recheck current security/version
advice and use the smallest C adapter that serves the protocol. Prototype under tools/tests, not a new
default backend flag. Exercise short writer transactions, concurrent readers, busy/cancel handling,
checkpoint/WAL growth and online backup. Port locking/upsert semantics explicitly, never mechanical REPLACE.

Compare a consistent export with SQLite at original-byte/hash, stable-ID, sequence, saved-revision,
source-range, status, consent, tombstone and pending-job level. Include file/DB reconciliation: SQLite
does not atomically commit independent manuscript files. Interrupt before/after the same transition points.

For retrieval, hold embeddings/chunking/tokenizer/query/fusion fixed. First compare existing Qdrant ANN/exact,
then eligible-array exact and released sqlite-vec. Eligibility must apply before top-k; reject outer-filter
shortcuts. Hydrate authoritative source text from SQL and re-admit at dispatch. Cover both older observer
indexing and newer memory protocols before retiring Qdrant. Try released USearch only if exact search misses
the preregistered scale budget. Reject an index swap with immaterial total savings or more recovery code.

### C07/C08 — platform mechanisms and surgical libraries

Prototype a small C/Objective-C NSTextView island inside the raylib/Clay shell, not the whole libui-ng toolkit.
Choose one editing/undo authority. Prove incremental UTF-16/codepoint/UTF-8 mappings, exact bytes, proposal
anchors, commit/cancel composition, dead keys, mixed scripts, focus/shortcuts, DPI and long-document behavior.
Real VoiceOver/IME runs remain necessary even if scripted tests pass; surrounding custom controls are not
made accessible merely by embedding a native editor.

Use utf8proc only for a distinct derived-key/property need, with collision-aware migration and unchanged
original bytes. Do not install three overlapping segmentation/shaping libraries. Trial already-linked
libcurl behind `http.h`; preserve auth, endpoints, SSE/caps, absolute deadlines and prompt idle cancellation.
If maintained code or measured whole-system cost does not improve, keep the current implementation.

### C09/C10 — quality and promotion

Borrow bounded literary name-resolution ideas before adding a Python stack or fourth resident model.
Keep generator/prompts/canon/judge budget fixed during ablations. Score false merges, abstentions, pair reach,
conflict recall/precision and foreground/backlog cost on each held-out book. An additional classifier must
justify its complete runtime, tokenizer and model-license costs. No automatic canon mutation.

Promote only after component-specific gates, real workload evidence, licence/model-rights closure and clean
offline install/restore. Migration first makes a separate target; the old authority remains recoverable.
Switch authority once, reconcile/recover interruptions, then retire the redundant default adapter/service.
Archive prior experiments without retaining a permanent backend matrix in the shipped app.

## Decision and stop rules

1. Safety is disqualifying: any lost/changed original, cross-source resolution, scope leak, automatic canon
   mutation or silent cloud fallback stops promotion regardless of performance.
2. Efficiency is net: record removed code/dependencies/services alongside the new adapter and recovery burden.
3. No framework may replace a working narrow mechanism merely because it is popular or comprehensive.
4. A failed component experiment is a valid result. Record it and return to the baseline; do not expand the
   rewrite to rescue the hypothesis.
5. Before irreversible real-data migration, dependency/licence-policy changes or a new quality budget,
   present the evidence and obtain the author's decision. Routine isolated implementation/testing can proceed.

## Resume protocol

Start with `AGENTS.md`, this ledger, and the journal's **Current handoff**. Inspect `git status` and diff;
uncommitted audit/research/architecture files predate implementation and must not be discarded.
Read the current slice's source/tests before editing. Record one slice's red test, change, green gates,
unrun gates and next exact action. Keep this table and the journal synchronized.

Validation follows CONTRIBUTING: `make test analyze sanitize`, plus TSan for threaded code and relevant
service/UI/crash/real-model gates when affected. Database suites run serially on throwaway stores only.
A missing display/service/model is a recorded blocked/unrun gate, not a pass.
Do not auto-commit a mixed working tree; leave explicit changes and evidence ready for review.
