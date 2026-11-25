# COMPOSITION-001 progress and handoff

Canonical work ledger: [COMPOSITION-001](../docs/COMPOSITION-001-plan.md).

## Current handoff

- **Active P0:** lightweight composition, authorized 2026-11-25.
- **Completed slice:** C01 — file chat failure guards / retry acknowledgement (AUDIT-005 F1/F3),
  mechanically verified, not a claim of real UI or DB acceptance.
- **Next action:** C02 design: inspect revision-key generation and every legacy lookup, rename, import/export
  and source-reference consumer. Write compatibility/ambiguity rules and red synthetic regressions before
  changing the revision format. Retain all C01 evidence; no real project migration.
- **Baseline:** `69b98abf1ecf86c560b435a209327175c182eab1`.
- No production migrations, dependency pin changes or commits authorized by this plan's first slice.
- Prior uncommitted docs/architecture/audit/research artifacts are session work to retain.

## 2026-11-25 — plan decisions

### Scope and options

The author promoted the component-composition research to P0 and delegated routine execution. The research
identified several independent opportunities, not an approved bulk replacement: embedded SQLite/FTS5 can
remove database operations; embedded exact vectors may remove Qdrant; native AppKit text can replace custom
input/layout duties; utf8proc can replace narrow Unicode logic; existing libcurl can consolidate transport.
Each alternative still needs comparative measurements, license review and the same safety contracts.

The baseline already reuses mature libraries. Its custom authority/provenance and source-admission code is
not disposable plumbing. Model allocations may dominate service/index savings. An embedded store changes
writer concurrency and migration/recovery behavior. A native editor changes index units, composition and
undo ownership. Bringing in all candidate libraries at once would make attribution and rollback impossible.
Keeping every backend indefinitely could increase precisely the code burden this work aims to reduce.

Open decisions before promotion: Which exact-search scale exceeds the agreed latency budget? Does a native
text island preserve source anchors and real input while removing more code than its bridge adds? Can
SQLite's writer contention and backup/recovery meet the whole-workload contract? What is the actual net
process/model memory difference? Which packaging/model-rights obligations remain? These are experiment
questions, not reasons to delay the first deterministic safety repair.

### Constraints and dependencies

1. Original bytes/identities and explicit author authority are invariants.
2. F1/F3 are caller persistence-state defects, independent of future backend choice.
3. F2 needs a compatibility design because old directory collisions can already be ambiguous.
4. Runtime advisories need reviewed fixes/precision gates, not blind latest-version adoption.
5. A controlled baseline precedes cost claims; a safety regression blocks promotion.
6. Storage/retrieval can be tested separately; Qdrant has two distinct indexing protocols.
7. Native editing is independent of SQL selection but must share one edit/undo authority.
8. Added neural extraction is last, after deterministic literary/Unicode improvements.

### Decision

**Remove responsibilities only after a replacement proves cheaper end to end and preserves the author contract.**
Start with the small caller repair rather than bundling it into a database migration. Keep current
services as controls while prototypes stay disposable. Put session state in one canonical ledger and this
handoff, with entry-point pointers instead of several competing plans.

### Adversarial checks and refined state model

- “Fewer daemons is necessarily lighter” fails if recovery/adapter code or model allocations dominate.
  Require net system measurements and permit rejection.
- “A file retry is a DB save without a sequence” fails if it activates a DB window or fabricates sequence 0.
  Model acknowledgement and sequence assignment independently.
- “All unknown sequences are unsaved” fails for normally loaded file chats.
  A file-backed guard checks actual failed appends, while a DB window additionally checks missing sequences.
- “Clear failure on any retry” fails on refused/uncertain writes. Clear only acknowledged success;
  retain failure state otherwise. Do not add an automatic retry loop in this slice.

| Event | Failure mark | DB sequence/window |
|---|---|---|
| Normal loaded file message | clear | unknown / inactive |
| Refused append | set | unchanged |
| Acknowledged file append | clear | unchanged; no invented sequence |
| Acknowledged DB append | clear | assigned / active |
| Retry fails | retained | unchanged |
| Retry runs after acknowledged success | no append | unchanged |

Implementation/test results follow below; the planning checks are design outcomes, not runtime evidence.

### C01 implementation and initial evidence

- Added headless regressions for file-backed failure visibility, refusal to replace/send through an unsaved
  conversation, invalid acknowledgements, DB-window distinction and repeated retry duplication.
- `chat_window_info` now exposes actual failed appends in either backend. `chat_mark_saved(..., -1)` clears
  an acknowledged file append's failure without inventing DB identity; DB acknowledgements retain their
  existing window/sequence behavior. Callback, adoption and explicit retry paths acknowledge both backends.
  Failed-save messages cannot be trimmed merely because they carry an older known sequence.
- `make build/test_llm > journal/composition001/c01/red-build.log 2>&1 && tools/watchdog.sh 300 composition-c01-red ./build/test_llm > journal/composition001/c01/red-test.log 2>&1`
  exited **1** against the original implementation; the build succeeded and regression assertions failed.
- `make build/test_llm > journal/composition001/c01/green-build.log 2>&1 && tools/watchdog.sh 300 composition-c01-green ./build/test_llm > journal/composition001/c01/green-test.log 2>&1`
  exited **0** after the fix: **5,111 checks, zero failures**.
- `git diff --check` exited **0**.
- Running gates (completion/exit status pending; do not duplicate):
  - `SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make test > journal/composition001/c01/test.log 2>&1`
    — completed **0**. Optional observer-branch check skipped because that branch is absent.
  - `make analyze > journal/composition001/c01/analyze.log 2>&1` — completed **0**:
    **zero warnings across 113 source files**.
  - `SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make sanitize > journal/composition001/c01/sanitize.log 2>&1`
    — first attempt **2**, documentation gate only; corrected rerun **0** (see below).
  - `make tsan > journal/composition001/c01/tsan.log 2>&1` — completed **0**; all reported
    checks passed, including **5,111** in `test_llm`; no ThreadSanitizer warning reported.
- Explicit exclusions: private evidence archive and untracked-novel yardsticks. Real GUI disk-full,
  VoiceOver/IME, database/crash/UI-db and real-model gates have not run. Mechanical success is not UI or
  model-quality acceptance. No services or real author stores were migrated; no commits were made.

### Gate correction

The first `make sanitize` invocation exited **2** at `evidencecheck`, before executing the C sanitizer suites:
the journal's exact commands cite the `test_llm` executable, which was not classified as a non-evidence build
artifact. Added that specific binary to `tests/fixtures/evidence_not_archived.txt`, consistent with existing
test/tool-binary entries. This does not exempt any result or log. Preserve the original `sanitize.log`;
the corrected rerun uses `sanitize-rerun.log`.

`SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make sanitize > journal/composition001/c01/sanitize-rerun.log 2>&1`
completed **0**. ASan/UBSan suites passed with the declared archive/yardstick exclusions and optional
observer-branch skip. No sanitizer finding was reported.

`make all > journal/composition001/c01/build-app.log 2>&1` completed **0**, verifying the production
callback/adoption translation unit and app link in addition to the headless suites.

### C01 disposition

Mechanically verified: focused red/green, full `make test`, `make analyze`, `make sanitize`, `make tsan`
and `make all`. Changes remain uncommitted. This resolves the demonstrated state/retry behavior in the
headless tests, not exactly-once persistence after an ambiguous write. Real UI failure handling and
isolated database/crash/UI-db suites remain unrun. Do not infer acceptance of those workflows from this status.
