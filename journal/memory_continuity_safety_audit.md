# Continuity safety audit — 2026-11-07

Scope: code review of the experimental original-only presentation changes and
brief-failure fallback. This is not security certification, model qualification,
or approval for default-on memory. The author's sample edit is untouched.

## Confirmed defect and repair

Final original-chat admission checked scope epochs, but did not freshly validate
each chat source. Real SQL reproduction: capture a valid original packet, change
its source message's `via` to `remote` without changing scope, then admit the old
packet. Admission incorrectly succeeded.

Evidence: `build/memory-safety-admission-before.log` records the failed rejection
assertion (246 checks, one failure). The repaired path rehydrates each selected
chat source using the existing SQL eligibility/hash/scope query and compares its
identity, role, position, hashes, token count, and exact text to the packet being
sent. Missing, changed, ineligible, or failed hydration rejects admission. Fresh
bodies are freed immediately. This adds at most eight bounded SQL hydrations,
not new candidates, source access, model calls, or a new memory tier; existing
preparation deadline/cancellation checks still apply.

`tests/test_memory_chat.c` now checks eligibility changes *after* retrieval,
restoration, role changes, and consent revocation. Its all-presentation controls
also cover malformed-brief fallback, initial source ineligibility, disabled
consent, canceled preparation, and edit bypass. The deterministic planner's
mixed-source double now derives kind from stable identity rather than hydration
call count, so repeated validation returns the same source kind.

## Reviewed boundaries

| Boundary | Code and evidence | Assessment |
|---|---|---|
| Remote routing | `chat_select_remote`, `chat_memory_configure`, `build_in_worker`, `chat_request_history_copy`; existing remote/provenance tests | Selection clears local memory routing; worker branches before retrieval; Sources metadata is not copied into model history. Presentation modes do not grant consent. |
| Edit routing | `build_in_worker`; new all-presentation edit test | Explicit passage editing bypasses retrieval and produces no memory receipt. |
| Source authority | SQL hydration; `memory_plan_admit_with_documents`; real-SQL regression above | Repaired final original-chat validation; document references and brief inputs retain their existing independent admission checks. |
| Citation binding | `memory_plan_apply`, `reference_for`, `reference_full`; compact receipt/final-fit tests | Short aliases and full receipts use the same final ordered source list. Receipt overflow drops optional material and rebuilds both views; current author text is not clipped to fit memory. |
| Ownership | `memory_plan_free`, staged brief additions, receipt `strdup`, request-build failure cleanup | Plan owns rendered text and receipt; session owns its independent receipt copy. Brief-only failure leaves raw ownership intact. Failed request construction clears the target receipt. |
| Cancellation | generative gate, preparation budget, final cancel check; native cancellation regression | Tested cancellation before admission leaves no Sources. This does not prove every possible post-admission race or real streaming cancellation timing. |
| History | `chat_pack_history_request`; exact-content and allocation-failure tests | Consecutive historical author turns are grouped in order; assistant turns and separate current request remain distinct. No invented assistant turns. Successful fault-injected packing must match the full expected packet. |
| Default-off | `src/main.c` environment wiring | Presentation selection is conditional on the memory experiment. T1 has a separate opt-in; no defaults or scope were broadened. |

## Important limits

- Admission is a point-in-time check, not a transaction held across inference.
  Revocation cannot erase material already sent to a model; this audit does not
  claim instantaneous revocation after admission or eliminate all concurrent
  mutation windows. No SQL locks should cross HTTP generation.
- Sources are historical as-of receipts, not canon or proof that a generated
  answer obeyed them. Existing persisted receipts are not retroactively erased
  by disabling recall.
- Remote filtering prevents automatic export of local-only history and Sources
  metadata. It is not a semantic scrubber for remembered information an author
  explicitly pastes into a remote turn, nor for explicit attachments they send.
- Delimiter neutralization and reference labels do not guarantee resistance to
  semantic prompt injection or speaker confusion. Author-intent fidelity needs
  full-response model review, separately from mechanical safety tests.
- The extra final SQL work remains inside the experimental 500 ms budget, but
  its real-model latency/resource impact has not been remeasured.

## Validation at this checkpoint

- `make -j8 all test dbtest`: passed after repair and expanded controls;
  `build/memory-safety-full.log` (native memory chat: 280 checks, zero failures).
- Normal and allocation-fault planner tests passed;
  `build/memory-safety-planner-tests.log`.
- Persistence allocation-fault suite, including history packing: 985 checks,
  zero failures, also included in the full test invocation.
- `git diff --check`: passed.
- AddressSanitizer/UBSan normal tests and real-DB tests passed:
  `build/memory-continuity-{sanitize,asan-db}.log`.
- TSan passed: `build/memory-continuity-tsan.log`.
- UI/store-UI, backup/recovery, remote stand-in provider and case-sensitive-volume
  suites passed: `build/memory-continuity-{ui,ui-db,backup,remote,case-sensitive}.log`.
- History, pipeline and history-worker mutation suites passed. The pipeline
  suite kills a new mutant that removes fresh original admission. The worker
  suite initially stopped because its connect-lock mutation targeted an obsolete
  unbounded-only connection statement; retargeting the same lock-across-connect
  defect to the current bounded/unbounded statement restores the check, which
  kills the mutant. Logs: `build/memory-continuity-*-mutations.log`.
- Clang static analysis of `src/llm/{chat,context_planner}.c` reported no findings:
  `build/memory-continuity-static-analysis.log`. This is a targeted check, not
  an all-code static-analysis claim.
- After adding deterministic admission-freshness cases, normal and ASan planner
  and planner-fault targets were rerun successfully:
  `build/memory-continuity-final-planner.log`.
- Post-repair native replay `build/mn-02ofw5l8` completed all 42 answers with
  Sources, matching cohort source IDs, preserved requests and no answer errors.
  Eight answers truncated; the mechanical report remains failed. All 28 compact/
  episode requests and answers match the earlier run exactly. This validates
  admission on these cases under unchanged bounds, not semantic qualification
  or a preparation-latency SLA. Hosted acceptance remains pending.

## Frozen native run and review discipline

The completed run `build/mn-z0h05o1h` predates this admission repair. All 42
answers ran, Sources were present, selected IDs matched across presentations,
and current requests were preserved. `report.json` reports
`MECHANICAL_GATE_FAILED`: at least one answer errored or truncated. It remains
negative DEV evidence, not a post-repair validation run or held-out result.

`tests/fixtures/memory_native/review-rubric.json` operationalizes the existing
frozen clauses without changing tasks, thresholds, sampling, or preregistration.
Review must separate mechanics, admission, facts, author intent, unresolved
state, current-task completion, citations, and whole-response completion. An
early correct sentence does not excuse a later contradiction or truncation.
The complete 42-response semantic review is recorded in
`memory_native_dev_review.json` and `../docs/MEMORY-011-native-continuity-dev.md`:
no presentation clears the frozen screen. T1/T2/default-on remain blocked;
no generated-aid work is authorized by this audit result.
