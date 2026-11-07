# Native continuity DEV: no-go

**2026-11-07. Development diagnostics, not held-out acceptance or an MVP claim.**

## Decision

None of the three original-only presentations clears the frozen small native
continuity screen. Stop advancement to changed episode selection, extractive
bookmarks, redesigned T1, or new held-out acceptance. Keep memory default-off,
T1 separately opt-in, and T2 deferred. GAP-003 P0 and other product gaps remain
outside this work.

This is a capability boundary for the **tested model/protocol/configuration**,
not a claim that the model cannot write, cannot answer any fact, or could never
work with a different separately authorized workflow. Correct source retrieval,
valid receipts, more memory tiers, and a larger answer allowance do not by
themselves establish faithful continuity of author intent.

## Frozen experiment

- Seven DEV tasks × alternating/repeated consecutive-user recent histories ×
  legacy/compact/episode presentations = 42 native `ChatSession` answers.
- Pinned local BF16 model, production sampling structure with temperature zero,
  repetition penalty 1.05 and prompt cache enabled; effective context 8,192
  tokens, 1,024-token reply allowance. These are **not** V2's 128-token settings.
- Same three original episode turns in SQL in each presentation cohort, followed
  by 64 recent filler turns. Generated answers are not saved into later history.
- Discovery uses a diagnostic frozen eligible SQL candidate pool. SQL hydration,
  context fitting, admission, native streaming and postprocessing are real. This
  does **not** measure Qdrant candidate discovery or whole-manuscript retrieval.
- Existing bounds remain: 32 candidates, eight original excerpts, four optional
  briefs, shared 16 KiB/4,096-token optional references, 16 KiB Sources receipts,
  and the experimental 500 ms memory preparation budget. No T1 was used.

The original run is `build/mn-z0h05o1h`; manifest, events, driver/server logs and
mechanical report are retained there. It completed 42 answers in approximately
724 seconds. Every answer admitted Sources, all cohorts had matching original
IDs, and current author requests were intact. Nine answers truncated; there
were no recorded answer errors. Accordingly the automatic mechanical report
is `MECHANICAL_GATE_FAILED`, not a semantic pass.

## Complete-response review

All complete visible answers were manually reviewed by the assistant against
the frozen expected clauses. This is not independent human adjudication.
`journal/memory_native_dev_review.json` records every response's axis judgments,
literal evidence quotes, rationale, and hashes binding the original event file,
manifest and rubric. The review separates mechanics, admission, factual fidelity,
author intent, open state, current task, citations and completion. Uncertain
cases do not advance; successful source admission does not excuse wrong answers.

| Presentation | Clean pass / 14 | Fail | Uncertain | Truncated |
|---|---:|---:|---:|---:|
| Legacy | 1 | 13 | 0 | 6 |
| Compact | 3 | 10 | 1 | 3 |
| Episode | 7 | 6 | 1 | 0 |

These are strict **compound DEV screen judgments**, not estimates of ordinary
writing-session factual accuracy. Repeated filler histories are adversarial;
the alternating condition is a control, not a representative user study.

Episode packing fixes several repeated-history failures: the count of 58,
unaccepted cobalt suggestion, rejected ferry plan, and preserved fire ambiguity
are recovered in the consecutive-user layout. It does not qualify the candidate:

- **Speaker attribution:** an alternating answer says “I did not decide” about
  the author's coat-color decision.
- **Deliberate ambiguity:** the staircase recap calls the bell's reality a
  “choice ... still pending”; the fire answer plans a later reveal of who lies,
  despite the author's instruction not to settle it.
- **Citation fidelity:** answers cite the host's reference header as S1, or invent
  a namespace extending through S64/S65 although only three originals exist.
- **Current task:** the requested two-sentence thank-you becomes five sentences
  of mixed letter/meta-commentary. Warm drafting itself is allowed; generic
  novel prose is not falsely scored as fabricated remembered history.

The compact/episode alternating rejected-plan answers correctly choose train
over ferry but ask whether to retain or discard the already discarded alternative.
That may concern historical-note retention rather than reviving ferry, so both
are explicitly **uncertain**, not exaggerated into definite transport reversals.
Even granting both a pass would not change the no-go.

### Context cost is not a free win

Measured native prompt-token ranges across the seven tasks:

| Recent history | Legacy | Compact | Episode |
|---|---:|---:|---:|
| Alternating | 1,158–1,196 | 999–1,034 | 999–1,034 |
| Consecutive user | 1,332–1,364 | 1,169–1,204 | 1,631–1,666 |

For consecutive-user history, grouping reduces the request to three messages
but **adds 462 prompt tokens** relative to compact because it explicitly labels
each historical author turn. It is a role-layout experiment, not demonstrated
compression. Alternating compact and episode have identical request content;
their timing differs partly with fixed cohort order and prompt-cache reuse.
Wall time/TTFT are diagnostic observations, not isolated performance wins or
an SLA; complete generation token usage and service RSS were not measured.

## What changed in code, and what did not

1. Optional brief failure no longer suppresses independently valid originals;
   final source/scope admission still applies and fallback is visible.
2. `SCRIBODO_MEMORY_PRESENTATION=compact|episode` is an explicit experimental
   presentation choice, conditional on `SCRIBODO_MEMORY_EXPERIMENT=1`. Default
   is legacy. Compact model text and full as-of provenance receipts are separate,
   independently bounded representations of the same final selected sources.
3. Episode mode groups consecutive historical author turns deterministically.
   It does not invent assistant replies or rewrite the separate current request.
4. The safety audit exposed stale original-chat eligibility at final admission.
   Final admission now freshly rehydrates and compares each selected chat excerpt.
   A real-SQL fail-before/pass-after test covers a source becoming remote after
   retrieval; restoration, role change and consent revocation are also checked.

The frozen native run above **predates item 4**. It must not be cited as runtime
validation of that repair. No source-selection expansion, summary-schema change,
model change, automatic intent classifier, semantic ledger, or extra memory tier
was added. Neither environment flag grants source consent or remote export.

## Remaining validation and limits

Local code validation passed: normal build/test/DB, ASan/UBSan including real DB,
TSan, scripted UI/store-UI, backup/recovery, remote stand-in provider,
case-sensitive-volume tests, and history/pipeline/worker mutation checks.
Targeted Clang analysis of the two changed core files reported no findings.
The safety audit records exact logs and the corrected stale mutation target.
Hosted CI/trusted acceptance are pending.

Code validation and fresh native runtime measurement of the admission repair
are separate from semantic qualification. The repair adds at most eight bounded
SQL hydrations inside the existing preparation budget; a fresh run must establish
whether original admission still completes under that budget, without treating
its answer quality as fresh held-out evidence or tuning failures away.

The post-repair replay `build/mn-02ofw5l8` completed all 42 answers in about
716 seconds, with no recorded errors, Sources on every answer, matching source
IDs across presentation cohorts and intact current requests. Eight answers
truncated (five legacy, three compact, zero episode), so the automatic report
remains `MECHANICAL_GATE_FAILED`. Source snapshots match the repaired production
files. All 28 compact/episode requests and answers are byte-identical to their
pre-repair counterparts: the fix retained admission without changing those
semantic outcomes. Legacy requests contain fresh opaque SQL/unit identifiers
and are not byte-identical across runs; their answers must not be treated as a
matched-input semantic comparison. Post-repair events SHA-256:
`9fa4d7593fbe833e49863eb2c6cdf81fcf1fad8414a074c4bf6a2e3184c002c6`.

This checks native admission under the unchanged preparation budget for these
cases; it is not an isolated measurement of each preparation phase, a sustained
load test, or a latency SLA. No additional memory layer advances on this result.

No claim is made for native foreground preemption, sustained real-model use,
300,000-word stress, service RSS, general semantic prompt-injection resistance,
or instantaneous revocation after dispatch. Admission is point-in-time and no
SQL locks should cross HTTP inference. Historical Sources receipts remain as-of
evidence, not canon or retroactively erased source access.

References: [root-cause diagnostics](MEMORY-010-quality-bedrock.md),
[approved gates](../journal/memory_continuity_synth.md),
[safety audit](../journal/memory_continuity_safety_audit.md),
`tests/fixtures/memory_native/{dev,preregistration,review-rubric}.json`.
