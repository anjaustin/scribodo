# Exact live-path investigation after red-team remediation

**Baseline:** `2a3211f`, 2026-11-09. No production model, prompt or selection
policy change is part of this measurement.

## Contract to measure

Trace the same actual question through candidate discovery, SQL-bound source
identity, admission, final serialized request, provider SSE, accumulated content
and persisted visible reply. The earlier reconstructed probes cannot substitute
for this live trace. The expected answer phrase being present is not evidence
that the designated passage was found or that every generated assertion is true.

`tools/answer_live_capture.py` generates diagnostic-only chat/planner objects.
Their request builders, personas, search callbacks, fusion and admission code
remain unchanged. Native generation is neither replayed nor replaced. The
instrumentation adds private local recording and refuses incomplete capture.
It records SSE callback lines, not HTTP framing; instrumentation I/O can affect
timings and the run is not an isolated speed comparison.

The unchanged seven-question manuscript script runs with the pinned models,
seed 42, requested 16,384-token context, ordinary 512-token units/eight-unit
policy, and failed experiments disabled. Capture is under
`build/answer-live-latest/`; complete replies and the final SQL source snapshot
are collected under `build/product-memory-use/live-latest/` before owned DB
cleanup. Capture IDs are thread-local to each real foreground worker.

## Instrumentation checks completed before inference

- Four Python tests passed: instructions/request builders unchanged; provider
  call retained; recorded lines precede chat parsing; search/fusion unchanged;
  missing or ambiguous anchors refused.
- Native callback smoke test passed: exact observed SSE lines, unchanged content
  decoding, 0600 files, no overwrite, and refusal without a private capture root.
- The first smoke attempt failed because its manual session setup retained
  `chat_load_messages`' cancellation flag. Resetting that flag as ordinary
  `chat_send` does corrected the fixture. The failed log is preserved and is not
  counted as a successful instrumentation check.

## Prospective discriminating checks

1. Resolve every lexical/vector locator against the captured SQL snapshot.
   Compare scope, generation, hash, sequence cutoff and original bytes. Locate
   each question's designated answer passage separately from loose aliases.
2. Determine whether the designated source was never discovered, lost during
   fusion/budget admission, present only in a document preview, or actually in
   the retrieved packet. Preserve candidate ranks and the full final context.
3. Compare raw accumulated model content with the saved visible reply. Do not
   attribute raw model mistakes to display cleanup.
4. Use exact live income/Collins requests (indices 1 and 2, selected in advance)
   for independent-runtime diagnostics if needed. Compare the official disabled-
   thinking template and token IDs first. The existing reference environment
   and cached pinned weights are available; no remote inference is required.
   Other continuity failures already reproduced in the independent reference
   (MEMORY-012), but that does not establish parity on these manuscript inputs.
5. Any runtime comparison must use identical token prefixes and matched decoding,
   run reference and llama sequentially, and report distributions as well as
   greedy divergence. Full-input numerical parity and factual correctness are
   different measurements. An oracle or shorter diagnostic cap is not a passing
   native-answer test.

## Actual live outcome and source-to-answer audit

The native run finished with **25 script assertions passed, 3 failed**. These
are script assertions, not 25 correct answers. All six expected source phrases
occurred in the supplied packet. Seven requests, seven provider streams and
seven complete reply records were saved; all seven transports returned HTTP
200 with complete capture, no cancellation and no recorded error. Six streams
ended with `stop`; the middle-name stream ended with `length` and the persisted
reply is marked truncated. The owned disposable SQL database was dropped after
the message/unit exports (`result.json`: cleanup exit 0).

The requested 16K context was **not** used. The app's explicit memory warning
reduced it to **8,192 tokens per slot**; the last server initialization in the
append-only log agrees. `native-current-session.log` extracts the final server
session, beginning at the final initialization marker and ending at cleanup.
This is not a controlled comparison against earlier runs with 16K actual slots.

`tools/answer_live_audit.py` resolves the recorded locators and designated
passages against the final SQL snapshot, hashes the unit text, reconstructs
fusion from per-channel ranks, and rebuilds content from SSE deltas. Results:
`build/answer-live-latest/audit-v2.json` (input and auditor hashes included).

- **820/820 exported units** have matching text hashes and true exported SQL
  source-hash/source-byte comparison flags. Every captured lexical/vector
  locator resolves with matching text hash, scope and generation; all captured
  chat candidates satisfy their respective pre-history cutoff. Zero audit
  integrity failures. These flags are the SQL comparisons exported before
  cleanup, not a new SQL query against the deleted database. Candidate epochs
  were not captured, so this is not an independent audit of every admission
  predicate or of the entire vector collection.
- **5/6 designated factual passages** reached the final request as complete
  original units. Income is the exception: the opening-scene unit was vector
  rank 4, absent from lexical top-k, fused rank 9, and not admitted. The normal
  unit limit is 8 (`memory_store.h`); the planner truncates lower-priority units
  again to enforce its framed byte/token budget. Seven units remain in the
  final packet. Fusion rank here is reconstructed from the unchanged production
  RRF rule and captured ranks, not a captured hydration/eviction decision log.
- The income phrase check passes because a **different, later part08 passage**
  containing “four or five thousand” is admitted. This does not establish
  admission of the question's designated first-arrival passage. The final
  answer instead quotes Darcy's ten thousand and attributes it to Bingley.
- **Collins' exact passage is lexical rank 1, vector rank 6, fused rank 1 and
  present intact in the final JSON**: “has very lately given him a living.”
  The raw answer nevertheless uses unrelated dialogue about Elizabeth's age,
  attributing it to Collins. Candidate discovery/admission cannot by themselves
  explain this failure.
- The opening-scene exact unit is fused rank 1 and present intact. Raw output
  begins with the correct phrase but invents family relationships and events;
  its final answer says the park is available for sale. It emits an orphan
  `</think>` despite disabled thinking. Ordinary reasoning cleanup removes the
  preceding 3,517 characters: raw 5,618 → visible 2,101 characters. The remaining
  false summary was already in the provider's content. The discarded prefix
  was also false overall; restoring it would not repair the answer.
- Raw content equals the persisted visible reply for **6/7** questions. All
  reconstructed SSE content equals captured accumulated content. No separate
  `reasoning_content` characters were observed. Untagged self-talk in the
  middle-name answer is emitted as content; the truncated reply contains
  repeated unsupported claims before eventually saying no middle name is
  stated. This is not a clean abstention pass.
- The age, Ramsgate and Brighton replies contain the requested facts, but
  their complete replies still include wrong source labels/attributions. For
  example Brighton cites SOURCE 3 for wording actually supplied in SOURCE 1;
  Ramsgate calls its SOURCE 4 a part08 excerpt when that binding is part04.
  Phrase correctness is not full-answer fidelity.

Four synthetic auditor controls pass: clean fixture; tampered source/SSE
detection; future-chat/missing-locator rejection; and phrase presence not
being equated with designated admission. No production policy was changed.

## Independent numerical comparison

The parity tool now freezes the actual **8K** slot, launches two slots with
16,384 total context, checks the server's effective context, and refuses inputs
that would exceed it. The preselected live income/Collins inputs render to
**5,796 / 5,267 tokens**. Official and llama templates match byte-for-byte and
token-for-token. Cold, unpenalized greedy llama diagnostics generated 512 tokens
for each, stopping at the diagnostic cap. This deliberately differs from native
sampling (temperature .7, repeat penalty 1.05, 2,048-token reply limit); it is not
a replay of the native answer or a complete-answer gate.

Artifacts: `build/alp1/`. CPU F32 official-reference execution ran after llama
exited. The 512-token prefill/chunk control's maximum top-ten log-probability
gap was **0.0000162125**; all 14 position-alignment controls passed.

| Exact live input | Income | Collins |
|---|---:|---:|
| Candidate-prefix positions compared | 512 | 512 |
| Shared greedy prefix, before first divergence | 312 | 310 |
| Reference top-ten covered by llama top-64 at every position | yes | yes |
| Argmax disagreements over all teacher-forced positions | 2 | 1 |
| Maximum top-ten log-probability gap | 0.64239454 | 0.24018598 |
| Positions exceeding the pre-existing 0.12 bound | 12 | 5 |
| First divergence's reference top-two margin | 0.00085068 | 0.00968552 |

These are **not passing full numerical-parity results**: some differences exceed
the existing bound even though first divergences are near-ties. They are not a
measurement of native-sampling equivalence either.

To exclude the chunking method as the explanation, a second CPU F32 reference
control runs the entire prompt plus 511 candidate tokens in a **single forward,
without cache** (`tools/answer_live_reference_control.py`). It agrees with the
chunked reference at all 512 argmax positions on each case. Maximum top-ten
full-vs-chunk gaps are **0.00023937 / 0.00013733**. Full-reference-vs-llama maxima
remain **0.64244938 / 0.24020052**. Thus the observed larger runtime differences
do not come from the reference's chunked prefill on these inputs. Their precise
numerical cause remains open; neither Metal nor llama computation is certified
by this experiment.

There is nevertheless a narrower direct semantic result **before divergence**:
the shared income prefix attributes Darcy's Derbyshire estate to Bingley and
discusses Mr. Bennet's dialogue as Bingley's debts. The shared Collins prefix
mislabels quoted part02 material as SOURCE 3/4 (part04), although it eventually
names the living. Those shared errors are not unique to llama or Metal. This
does not mean the independent reference reproduced the native sampled Collins
age answer or the final native ten-thousand answer; that was not tested.

Distributions after a divergence are explicitly teacher-forced on the candidate
prefix, not an independently free-running reference answer. The complete-answer
gate remains failed regardless of numerical agreement.

## Live context-component controls

`tools/answer_live_ablation.py` uses actual Collins input, not the reconstructed
older request. It preserves the native system instructions, source bindings,
seed and repeat penalty; sets cold generation and temperature zero; uses the
actual 8K slot and full native 2,048-token reply cap. Predeclared arms: full
request; history removed; document previews removed; both removed (packet only);
and an oracle-selected intact original unit, preserving its source binding.
The last is a reading control, not a production retrieval solution. Artifacts:
`build/ala1/`. Four no-model transformation tests pass. Together with capture
and auditor controls, these protect component removal rather than certify model
answers.

All five model arms finished with `stop`, zero cached prompt tokens, and complete
saved replies. No arm exhausted its 2,048-token allowance:

| Collins arm | Prompt tokens | Completion tokens | Complete reply observation |
|---|---:|---:|---|
| Full actual context, cold greedy/native penalty | 5,267 | 447 | Wrong answer: home visit, dinners, shelves; omits the living |
| History removed | 5,000 | 278 | Names living; falsely locates quote in SOURCE 3 and adds wrong sibling/place claims |
| Previews removed | 4,247 | 312 | Names living; mixes in Elizabeth's age dialogue and calls it a grant at Rosings |
| Packet only | 3,980 | 414 | Names living; adds wrong living location and sibling/invitation claims |
| One intact, correctly bound oracle unit | 705 | 234 | Names living; invents a stitched quotation and calls it hereditary title/landed property |

The full arm establishes that temperature-zero decoding with the native repeat
penalty **does not fix** this case. The earlier unpenalized greedy numerical arm
did name the living (while misattributing sources); these are distinct decoding
conditions, not a before/after quality gain. Component removal changes whether
the core answer appears under otherwise fixed decoding, but none of these five
complete replies is clean. This is not just an inability to retrieve the word
“living,” nor does the oracle constitute a reliable reading/meaning pass.

The retained history for Collins consists of exactly the immediately preceding
income question and the erroneous ten-thousand answer. Removing it removes 267
prompt tokens. This measures sensitivity to removal, **not** a separation of
semantic contamination from length/position effects; there is no matched-length
neutral-history control in this run. Document-preview removal likewise changes
positions and length. These controls do not justify discarding author history,
changing source authority, or promoting an oracle selector to production.

## Bounded causal conclusions and verification

1. **Selection loss is observed after discovery.** Income's designated original
   exists, is vector-discovered, then ranks outside the admission unit cap. The
   phrase-only source assertion fails to detect the distinction between that
   passage and a later alias-bearing passage. No corrupted source is needed to
   produce this observed loss.
2. **Answer/source interpretation fails even after successful admission.** The
   opening and Collins originals arrive intact; their raw outputs invent or
   misattribute facts. Some source/entity errors also occur in the independent
   reference's identical greedy prefix. This excludes an explanation consisting
   solely of SQL/VDB corruption, display cleanup, or a llama/Metal-only semantic
   error. It does not exclude additional runtime defects or certify parity.
3. **Context and decoding matter, but local simplification is insufficient.**
   Live component removals change the core Collins answer. Even oracle evidence
   leaves invented quotations/definitions. Neither “more recall,” “greedy,” nor
   “single-source” is established as a complete remedy.
4. **The existing script is too weak as complete-answer acceptance.** Three
   factual replies fail its phrase assertion; others pass with incorrect source
   attribution; the absent-fact question only checks that generation is idle.
   It therefore passes mechanically despite a long, truncated, self-contradictory
   reply. We have not relabeled these as reliable answers.

Production source comparison: **169 files, zero drift** against both live-capture
hashes and committed baseline `2a3211f` (`source-drift.json`). Changes in this
investigation are diagnostic tools, tests and this record; failed experimental
production flags remain off. CI configuration is untouched. No product-quality
or 100/100 claim is justified.

The remaining numerical gap's cause is open. Resolving it requires additional
matched-prefix arithmetic/backend controls; first-divergence near-ties alone
cannot waive the measured bound. A production remedy must then pass complete
replies on fresh questions, not just replay these words or improve a source
phrase counter.

Final diagnostic verification: **16/16 no-model tests passed** (capture, audit,
ablation transformations and comparison-prefix/length controls);
`git diff --check` passed. No `llama-server` or Scribodo app process remains.
The initial native callback smoke is recorded above; broader production gates
were not rerun because production sources did not change. Investigation changes
are uncommitted; this record is not a new pushed release or product acceptance.

## Subsequent Lincoln Manifold pass

See [answer-manifold_synth.md](answer-manifold_synth.md) and its phase records.
Twenty new predeclared diagnostic model arms refine, rather than erase, the
findings above. Token-matched neutral history still fails Collins; a minimal
one-sentence instruction yields a correct full-context answer, replicates, and
beats equal-token non-directive suffix controls on that input. The same rule
does not fix income, citation fidelity or absence handling across the other
captured questions. Synthetic counterfactual output follows a changed gift,
demonstrating narrow source-following capacity, not reliable general execution.
Actual native C text cleanup corrected one raw-only quote failure classification.
No production remedy was promoted, and the numerical gap remains open.

## Plumbing follow-up: qualified controls and remaining boundaries

See manifold turbo cycles 5-7 and follow-up qualification for full artifacts.
Strict CPU requires `--device none --no-op-offload --no-kv-offload`; the abandoned
ap1 control did not enforce this. Strict ap2 completed 24 one-token controls,
showing backend/cache sensitivity, not a demonstrated cause of native sampled
answer failures. These continuations are diagnostic greedy prefixes.

The diagnostic token-count API defect (BatchEncoding field count versus token
count) was repaired; historical ala1/amc1 addenda independently match saved
provider usage. Production C token counting is unaffected. pc1 cold native
Collins/middle-name responses replay the actual live raw output byte-for-byte.
Vendor-core sampling plus thinking fixes Collins on this input but fabricates
an absent name, so no default bundle earns complete acceptance.

pr1 confirms output-cap truncation in the original two-8K-slot configuration:
cap 2,407 permits the exact native reply to finish at 2,359 tokens. Prompt plus
completion plus reserve is 8,144. Native cleanup reduces the completed content
to 793 visible characters; source/support errors remain. pr1/pt1 official
template bytes, provider counts, cold accounting and cap-prefix checks pass.
pt1's completed thinking-only Collins still gives the wrong shelves/approval
answer after 6,226 tokens; middle-name abstains after 5,012 but adds false support.

pq1's strict-CPU first request was still generating (last logged count 2,025)
when the 600-second nonstreaming client timed out. No full response was captured;
the second request never ran. This is an incomplete control, not evidence of
CPU answer quality or a production streaming timeout defect. Production HTTP
refreshes its inactivity deadline when bytes arrive. No production defaults,
CI configuration or pinned model were changed by these experiments.
