# COMPOSITION-001 C03 — runtime security and dependency triage

**Started:** 2026-11-26. **Baseline:** `4d380a4`.
**State:** isolated candidate builds, ordinary upstream tests and first-pass MODEL-001 parity pass.
REDTEAM-028 acceptance-tooling repairs and extended live adapter/role/mechanical gates pass.
Resource and final adoption gates remain open.
No product pin, model, precision or schema change.
**Ledger:** [composition001-progress](../journal/composition001-progress.md).
**Primary-source evidence:** [C03](../journal/composition001/c03/README.md).

## Current findings

The refreshed committed-pin report exits **1**: four llama.cpp advisories and the previously moved
MariaDB tag. Update availability for unrelated dependencies is informational, not permission to upgrade.
The original two-advisory AUDIT-005 observation remains historically correct; two newer records now match.

| Advisory | Required input / affected code | Observed product boundary | Disposition |
|---|---|---|---|
| CVE-2026-133710 | Crafted `hrm_text` GGUF metadata reaches overflowing slot-count arithmetic, then out-of-bounds layer indexing | Stock launch selects pinned LFM3/Qwen3 artifacts and verifies their configured hashes before `posix_spawn`. This is an artifact admission boundary, not absence of the vulnerable loader from the binary | Fix required in the runtime; no untrusted GGUF is loaded for triage |
| CVE-2026-134540 | Invalid numeric `grammar_triggers[].type` reaches the sampler's assertion | Native completion schema accepts raw trigger objects. Scribodo builds structured chat requests, not author-controlled top-level trigger objects; the supervised endpoint uses a private Unix socket and API key | No anonymous TCP attack route in the inspected stock launch; authenticated/native-API capability remains in the runtime |
| CVE-2026-137057 | GBNF repetition with maximum below minimum reaches grammar parser underflow | Native completion API accepts grammar strings. Product chat/observer/answer requests use messages and application-built schema/template options; no author-facing raw GBNF field was found | Same socket/auth boundary; generated grammar and its parser still require independent regression tests |
| CVE-2026-137567 | Malformed tensor-copy request to a ggml RPC server | RPC defaults OFF upstream, is OFF in the actual existing CMake cache, and is not requested by Scribodo's launcher | Not exposed by the inspected build and launch configuration; retain explicit OFF for candidate testing, not a universal claim about other builds |

These are source-traced boundaries, not a completed penetration test or a claim of unconditional
unreachability. Hash verification is cached by path/size/mtime/inode; same-user filesystem replacement,
cache tampering and hand-launched/custom servers are not ruled out by the allowlist observation.
No exploit was sent to any live server.

Source entry points inspected:

- `src/llm/server.c`: stock artifact hashes/configuration, `verify_model`, `write_key`, `spawn_server`;
  `0700` run directory, `0600` API key, Unix-socket host and `-fa off -ctk bf16 -ctv bf16`.
- `src/llm/chat.c`, `observer_model.c`, `answer_pipeline.c`, `memory_brief.c`, `names.c`:
  request serialization and application-owned JSON schema fields.
- Pinned upstream `tools/server/server-http.cpp`: API-key middleware precedes handlers, excluding
  health/public endpoints and preflight OPTIONS; `server-schema.cpp` / `server-common.h` accept the
  native grammar/trigger fields.
- Pinned upstream `common/sampling.cpp`, `src/models/hrm-text.cpp`, `ggml/CMakeLists.txt`:
  actual vulnerable code and RPC build default. Finding a CVE match is not proof of an active listener.

## Reviewed upstream fix ancestry

| Advisory | Fix commit |
|---|---|
| CVE-2026-133710 | `50518b64c6f2191a116d2760042f106c4b3387d7` |
| CVE-2026-134540 | `9f26fe11c73c7e74a5e99e7f1b6370ad8e1cb22d` |
| CVE-2026-137057 | `46eb1742db6d2caec1bfa848c5702b1bdcc815e5` |
| CVE-2026-137567 | `03813c56e47f6cd7a9b9bc2ad8edcebed88ce342` |

All four fix commits and the current pin are ancestors of candidate
`03813c56e47f6cd7a9b9bc2ad8edcebed88ce342` (four `merge-base --is-ancestor` checks, exit **0**).
This candidate is the latest of these four fixes, not a claim that current upstream HEAD is acceptable.
It is **542 commits** beyond the product pin: 2,654 files change, including LFM3 execution, Metal
kernels/fusion, Jinja/template handling and server code. Much of the file count is unbuilt UI, but
the execution/template changes prohibit treating this as a behavior-neutral pin refresh.

The candidate is checked out detached in an isolated temporary upstream worktree. The vendored product
checkout and committed gitlink remain at `5851a8e505f4d5fbb183f2847698eb6155d176b7`.
No upstream commit, push, PR or in-place vendored edit is part of this task.

### Isolated build, linkage and selected-kernel review

The first candidate build linked Homebrew OpenSSL 4, unlike the baseline. Reconfiguration with
`LLAMA_OPENSSL=OFF`, rebuilding and the ordinary parser test exited **0**; final `otool -L` lists only
Apple/system libraries. RPC remains explicitly OFF. The product build recipe is unchanged.

LFM3's causal convolution uses `GGML_OP_SSM_CONV`, not the new half-staging 2D-convolution path.
The selected F32 SSM kernels are unchanged; the BF16/F32 matmul specialization retains its types.
The observed additional candidate pipeline is `kernel_rms_norm_mul_add_f32_4`, a `float4` fusion.
Source diffs and the per-runtime pipeline inventory are retained under `journal/composition001/c03/`.
This review is scoped to the selected LFM3 workload, not all supported architectures or kernels.

## D2 — MariaDB tag movement explained at manifest level

Both the pinned and current tag references are OCI multi-platform indexes, not a platform-child/index
mix-up. Immutable manifests/config blobs were fetched for Linux arm64 and amd64, and each response body
was checked against its digest. The pinned index is still retrievable.

| Property | Pinned image | Current `11.4.13` tag |
|---|---|---|
| Index | `sha256:5e73baf8179b29f115101bb102763a47a017361f3e9792daccba6646c3ab1532` | `sha256:a8992b5e5404d11e03c54a31d0c1bdea3fd116d283aa6d1a627551f29045309b` |
| Source commit | `bdfe641466a5312bb97d06a1a4e1fb411c49e6b6` | same |
| Reported MariaDB package version | `1:11.4.13+maria~ubu2404` | same |
| Base | Ubuntu noble, per-platform digests in retained index | Ubuntu noble, newer per-platform base digests |
| amd64 config creation | 2026-11-11 | 2026-11-20 |
| arm64 config creation | 2026-11-11 | 2026-11-20 |

For both inspected architectures, the runtime `config` objects are equal. History commands differ
only at the base filesystem ADD entry; all eight filesystem layer digests differ. This supports a
rebuild of the same MariaDB source/version on refreshed Ubuntu base layers, not an unreviewed MariaDB
version jump. It does **not** prove package-level equivalence, provenance authenticity or lack of a
vulnerability: base packages and apt-selected dependencies can differ on a rebuild.

Keep the current digest while runtime evaluation is the single active repair. A later image refresh
requires package/provenance review and isolated Docker DB/import/export/backup/restore gates; no real
store restore or migration is authorized. The dependency report therefore remains nonzero; this is not
being hidden or relabeled green.

## Initial license inventory and obligations

This is an inventory of inspected license texts, not legal approval or a complete release SBOM.

| Component | Inspected license | Obligation / remaining review |
|---|---|---|
| llama.cpp | MIT (`third_party/llama.cpp/LICENSE`) | Keep notices; review selected candidate's bundled linked dependencies and license delta |
| cJSON | MIT | Keep notices |
| raylib / Clay | zlib-style / zlib-libpng | Keep notices; mark modified sources if any (none changed here) |
| stb | MIT or public-domain alternative | Choose/document the distribution's option; keep applicable notices |
| MariaDB Connector/C | LGPL 2.1 text (`COPYING.LIB`) | Current app statically links it; relinking/source/notices obligations need release review, not dismissal as “just a library” |
| Pinned LFM3 model | LFM Open License v1.0, as reviewed in MODEL-001 | Revenue-based commercial-use condition and redistribution notices remain; downloading at first run does not remove use restrictions |
| Images, system/static libraries, fonts, other model artifacts | Not fully inventoried in this pass | Explicit outstanding release/SBOM work; no new license acceptance is inferred |

The candidate's top-level MIT license and `licenses/` tree have no delta in the captured comparison.
That does not excuse reviewing newly included source/dependency paths or the static link closure.

## Adoption gates — all required before a runtime pin change

1. Finish source/kernel review for the actual compiled BF16/F32 paths; build cleanly outside product output.
   RPC stays OFF; model artifacts, generation policy and local-only routing stay unchanged.
2. Run ordinary upstream grammar/sampling tests in disposable processes, not against a live author server.
   The newly prepared malformed-input fixture is unexecuted after reported tool safety errors; do not
   reroute it. Record that coverage gap explicitly rather than claiming adversarial rejection coverage.
3. Revalidate exact artifact hashes and `gguf_identity.py` tensor identity. Preserve `-fa off -ctk bf16
   -ctv bf16`; output similarity cannot establish precision.
4. Restore/check a reproducible reference environment. The historical interpreter is absent; an isolated
   Python 3.12.15 environment was restored offline from frozen project dependencies, including documented
   PyTorch 2.15.0 / Transformers 5.22.1. The full freeze is retained. This is not claimed byte-identical
   to the missing historical environment. Legacy helper sources and historical outputs remain unchanged.
5. Compare current pin and candidate with MODEL-001's reference-distribution thresholds (short <0.05,
   long <0.12, reference top-10 present), template/tokenizer/Unicode checks and captured production-shaped
   requests, including reasoning-history treatment and `enable_thinking=false`.
6. Run real-model adapter and affected fake/real UI/retrieval/observer regressions serially with isolated
   synthetic/public-domain data; measure resource/context/checkpoint behavior separately from quality.
7. Run the normal repository gates on the final bounded change; record pin provenance, license inventory,
   negative controls and every executed/unrun gate. Initial evaluation had no commit/push authorization;
   on 2026-11-27 the author authorized a reviewed, verified checkpoint and push. That is not automatic
   production pin promotion.

## First-pass compatibility result — 2026-11-26

The author explicitly authorized compatibility execution. The original serial command manifest is
archived with the run; the current `journal/composition001/c03/run-compatibility.sh` was subsequently
hardened by REDTEAM-028 to require explicit new paths. The original driver and every stage exited **0**.
Model fetches were disabled, with cached artifacts at their exact MODEL-001 revisions and an isolated
dynamic-module cache. Each runtime ran as an owned child on a private authenticated Unix socket,
with no pre-existing model server, and stopped before the next stage. No product app or store was used.

| Check | Baseline `5851a8e50` | Candidate `03813c56e` | Existing criterion |
|---|---:|---:|---|
| Four short distributions, worst reference-top-10 gap | 0.025735 | 0.022585 | <0.05, all reference top-10 present |
| Ten positions through 4,096 tokens, worst gap | 0.082297 | 0.083621 | <0.12, all reference top-10 present |
| Short greedy continuations | 40/40 ×4 | 40/40 ×4 | Descriptive, not precision proof |
| Template byte equality | 5/5 | 5/5 | Against pinned Transformers template |
| Ordinary document/Unicode tokenization | 9/9 | 9/9 | Exact token-ID equality |

Artifact verification matches **286** original tensors exactly, none reordered/missing/unmatched;
the GGUF has **154 BF16 + 133 F32** tensors, with F32 weights losslessly widened from BF16.
SHA-256 remains `39158a79f56edb85e0c4252ce6447b8d7883ef82ffedd02f0cda7c84da5e9f68`.
The reference parameters and long-context computation report F32.

Both runs explicitly use `-fa off -ctk bf16 -ctv bf16`, one slot/8K pool and product batch/microbatch/
checkpoint settings. Verbose logs report BF16 K/V and flash attention disabled; their 19/20 compiled
pipeline bases show BF16/F32 paths, with no F16 or quantized names. Pipeline compilation logs plus
source tracing support the exercised precision paths; they are not exhaustive GPU instruction tracing.

The five template cases include stripped prior reasoning and `enable_thinking=false`. They are
template-only checks, **not** execution of captured production chat/observer/schema requests.
The raw-reasoning diagnostic and literal role-marker tokenizer case from the legacy harness were not
included in this benign first pass. The malformed-input fixture remains uncompiled/unexecuted.
Ordinary upstream `test-sampling` and `test-grammar-parser` both pass (ctest exit **0**).

An independent offline postflight reproduced the gaps, validated complete finite reference distributions,
exact case counts, original source hashes, shared provenance and unchanged runtime binary hashes.
A new, non-MODEL-001 diagnostic initially asserted normalization within 1e-5 and failed: Python-F64
re-summing the stored F32 reference yields residuals up to **0.000111**. It is now reported, not used
as a new acceptance rule; no probabilities were renormalized and no established quality threshold
was relaxed. The residual's numerical cause remains unverified. The failed checks and corrected
postflight are retained rather than hidden.

Raw distributions, replies, logs and source/build snapshots are preserved in the **201-file** local
archive listed in [EVIDENCE](EVIDENCE.md), SHA-256
`201847a31561b2c9383ad059eb52e268df19e1e330e2e949c9e2c069bd61ffc1`;
every member was rehashed on read-back. `parity-archive.json` is the receipt.

**Still required:** captured production-shaped requests, multi-slot/context/checkpoint and whole-system
resource comparisons, Qwen embedding/reranker compatibility, final linked-license closure, affected
adapter/UI/observer/retrieval gates, independent adversarial review/remediation and full repository
acceptance on a proposed bounded production change. No pin promotion is justified by this pass alone.

## REDTEAM-028 — 2026-11-27 checkpoint

The adversarial regression suite exposed **13 false-acceptance failures** in 15 initial cases: empty/
truncated evidence, omitted artifact/precision checks, mixed provenance and fabricated stored equality.
The measured original observations passed independent postflight, but the original main comparator
could not safely gate future changes. It has been repaired and integrated into `make test`.

The expanded **27-case** suite passes, including case substitution, output preservation, archive
collision/symlink refusal, bounded primary-source capture and owned-child cleanup on ordinary readiness failure.
All six stages of a fresh `parity-02` run exit **0**, with the same numerical results as the first pass;
the independent postflight also exits **0**. The old run/archive are unchanged.
The fresh runner records an explicit per-stage exit ledger and binary version identifiers.

Both real C adapters pass 24 checks; four schema/cache/slot extractions each are source-exact.
The hash-admitted Qwen embedding vectors and three reranker scores are identical between runtimes
on the frozen synthetic smoke inputs. Full tests, analyzer, ASan/UBSan and TSan pass serially.
These extend coverage, not the held-out workload/resource/independent adoption claims.

See [REDTEAM-028](REDTEAM-028-runtime-compatibility.md) for findings, scope and final disposition.
Do not read an accepted measurement-tooling checkpoint as a shipped runtime security fix.
