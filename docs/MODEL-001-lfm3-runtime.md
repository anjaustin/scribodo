# MODEL-001: LFM3-1.7B BF16 runtime on macOS

**Date:** 2026-11-04 · **Hardware:** Apple M4, 16 GB unified memory, macOS 26 · **SOW:** §4.5, §6, §8.9, §10 (feasibility gate)

**Result: ✅ Pass, with conditions.** The official BF16 weights run on the Metal GPU through llama.cpp and match the reference implementation, including at long context. Our first configuration (flash attention on) was **not** BF16 end to end: flash attention stages values in F16. The compliant configuration turns flash attention off, at a cost of 6–10% in speed and extra memory at long context. The design for running chat and the observer together must also include scheduling, or the observer stalls chat for seconds at a time.

This report was red-teamed in two passes, first by the author and then independently; see [Red-team review](#red-team-review). Several claims in the first version were wrong, and they're corrected throughout.

## Recommendation

Run **llama.cpp `llama-server`** as a supervised child process, reached through a **Unix socket**, with:

```
-m LFM3-1.7B-BF16.gguf -ngl 99 -fa off -ctk bf16 -ctv bf16 --jinja
-np 2 -c <2 × per-slot context> -b 256 -ub 256 --cache-ram 0
--host <0700 dir>/llama.sock --api-key-file <0600 file>
```

Before every launch, the supervisor checks the GGUF's pinned SHA-256 (`39158a79f56edb85e0c4252ce6447b8d7883ef82ffedd02f0cda7c84da5e9f68`). It removes any stale socket, waits for `/health`, and restarts the server if it dies. The chat adapter neutralizes special-token text in all content it sends, strips earlier reasoning from the history, and schedules observer work only while chat is idle.

## What was tested

| Artifact | Source (pinned) | Precision |
|---|---|---|
| `model.safetensors` | `LiquidAI/LFM3-1.7B` @ `0dc4268b` | 286 tensors, all **BF16**, 1,728,801,536 parameters |
| `LFM3-1.7B-BF16.gguf` | `LiquidAI/LFM3-1.7B-GGUF` @ `cd0c74c5` (Liquid's own conversion) | 154 BF16 + 133 F32 tensors |

**The GGUF is bit-identical to the original weights.** `tools/model_check/gguf_identity.py` matches all 286 original tensors exactly, with none reordered, unmatched or missing. The 133 F32 tensors (norms and 7×2048 convolution kernels) are all lossless widenings of BF16 values. As a negative control, the same script **rejects the Q8_0 GGUF**.

| Runtime | Version | Verdict |
|---|---|---|
| **llama.cpp** (Metal) | `5851a8e5` (master, 2026-11-04) | ✅ **Use, with `-fa off`** |
| mlx-vlm (Metal) | 0.7.11, mlx 0.32.4 | ✅ **Correct BF16 runtime** (pure BF16 compute); not chosen for serving reasons (see [Choosing between them](#choosing-between-llamacpp-and-mlx)) |
| mlx-lm | 0.32.2 | ❌ Doesn't load LFM3 yet (ml-explore/mlx-lm#2016) |
| Liquid's MLX build | `LFM3-1.7B-MLX-4bit` | ❌ 4-bit; excluded by the spec |
| Reference | Transformers 5.22.1 + Liquid's `modeling_lfm3.py`, PyTorch 2.15, CPU, **F32 compute** from the BF16 weights (SDPA attention) | Ground truth |

The remote model code was **scanned**, not fully audited: its four files import only `torch` and `transformers` internals, with no OS, subprocess, network, file or `eval` use. It runs at the pinned revision.

## Precision, end to end

**How to verify precision.** Output comparison **cannot** verify precision. Q8_0 passes every output tolerance that BF16 MLX passes (see the controls below), so precision has to be checked structurally:
1. **The artifact:** a pinned SHA-256, plus `gguf_identity.py` showing bit-exact BF16 weights.
2. **The launch flags:** `-ctk bf16 -ctv bf16 -fa off`.
3. **The kernels:** at runtime, only the kernels listed below run. Check with `-v` and the `base = 'kernel_…'` lines.

At default verbosity the server log doesn't show the weight or KV types, so the check has to read the artifact and the flags, not the logs.

**llama.cpp Metal kernels, as source-read and observed:**

| Operation | `-fa off` (recommended) | `-fa on` |
|---|---|---|
| Prompt matrix multiplies | `mul_mm_bf16_f32`: activations converted to **BF16**, BF16×BF16, F32 accumulation | same |
| Generation matrix multiplies | `mul_mv_bf16_f32`: BF16 weights × F32 activations | same |
| Attention scores and weighted values | `mul_mm/mv_bf16_f32` over the BF16 KV cache; **softmax in F32** | `flash_attn_ext_bf16`: Q/K/V BF16, scores F32, **output accumulator F16**; decode `flash_attn_ext_vec_bf16`: **Q/K/V staged as F16** |
| KV cache | BF16 (`set_rows_f32_i64_bf16`) | BF16 |
| Norms, RoPE, convolution, SwiGLU | F32 | F32 |

Every kernel with `-fa off` is BF16 or F32. **With `-fa on`, attention converts BF16 values to F16.** F16 has finer precision but a much smaller range (max 65,504, against ~3.4×10³⁸ for BF16), so that path isn't BF16 compute and could overflow on extreme activations. llama.cpp's KV cache also defaults to F16, so `-ctk/-ctv bf16` is always required.

## Correctness against the reference

**Metrics.** For each check position:
- **Top-1:** whether the most likely next token agrees.
- **Largest gap:** the largest log-probability difference over the *reference's* top-10 tokens, each of which must appear in the candidate's top 20.
- **KL divergence:** over the candidate's top-20, renormalized for llama.cpp; full-vocabulary for MLX.
- **Greedy match:** for short prompts, a 40-token greedy continuation.

Repetition penalty is set to 1.0 everywhere: `generation_config.json` defaults to 1.1, which causes false failures.

**Short prompts:** 4 prompts of 22–46 tokens (English chat, Unicode names, German, prose).

| | Top-1 | Greedy | Largest gap (top-10) | KL |
|---|---|---|---|---|
| **llama.cpp BF16, `-fa off`** | same ×4 | 40/40 ×4 | **0.013–0.026** | ≤ 1.7×10⁻⁵ |
| llama.cpp BF16, `-fa on` | same ×4 | 40/40 ×4 | 0.010–0.037 | ≤ 8.9×10⁻⁵ |
| mlx-vlm BF16 | same ×4 | 40/40 ×4 | 0.078–0.188 | ≤ 8.3×10⁻⁴ |
| *Control: Q8_0* | same ×4 | 40/40 ×4 | 0.075–0.129 | ≤ 8.2×10⁻⁴ |
| *Control: Q4_K_M* | same ×4 | 40, 18, 40, 30 | 0.62–2.62 | 4×10⁻³ – 4×10⁻² |

**Long context:** a 4,096-token mixed Markdown document. The check positions cross the **256-token sliding window** and run out to 4K. The short prompts never reached the window.

| Position | 200 | 255 | 256 | 257 | 300 | 512 | 1024 | 2048 | 3072 | 4096 | **Worst gap** |
|---|---|---|---|---|---|---|---|---|---|---|---|
| llama.cpp `-fa off` | 0.045 | 0.051 | 0.039 | 0.082 | 0.068 | 0.035 | 0.048 | 0.030 | 0.019 | 0.031 | **0.082** |
| llama.cpp `-fa on` | 0.063 | 0.043 | 0.057 | 0.040 | 0.033 | 0.068 | 0.048 | 0.027 | 0.048 | 0.071 | 0.071 |
| mlx-vlm | 0.112 | 0.239 | **0.443** | 0.354 | 0.173 | 0.114 | 0.119 | 0.159 | 0.120 | 0.169 | **0.443** |
| *Control: Q8_0 (`-fa on`)* | 0.420 | 0.132 | 0.276 | 0.234 | 0.113 | 0.109 | 0.094 | 0.125 | 0.115 | 0.065 | 0.420 |

All top-1 disagreements fall on **near-ties**, where the reference's top two tokens are within 0.003–0.064 nats (mlx-vlm at 200, 255 and 512; llama.cpp `-fa on` at 512). **Top-1 agreement is therefore not a useful signal; the distribution metrics are.** By those, llama.cpp stays within 0.082 of the F32 reference everywhere, while mlx-vlm reaches 0.44 at the window boundary.

**That gap is BF16 arithmetic, not an MLX defect.** Running Liquid's own reference code in BF16 instead of F32 moves it just as far from the F32 reference:

| Max top-10 gap over the 10 positions | vs F32 reference | vs BF16 reference |
|---|---|---|
| Liquid's reference in BF16 | **0.403** (median 0.195) | — |
| mlx-vlm (BF16 activations) | 0.443 (median 0.169) | 0.318 (median 0.220) |
| llama.cpp `-fa off` (F32 activations) | **0.082** (median 0.045) | 0.449 (median 0.218) |

Any runtime that computes activations in BF16 sits ~0.4 from the F32 result on this model, and two BF16 implementations differ from each other by ~0.2–0.3 because they round in a different order. llama.cpp keeps activations in F32, so it sits close to F32. **Both runtimes are correct.** The first version of this report, and the independent reviewer's ~1.9K-prompt observation, read MLX's BF16 drift as a correctness failure. That was wrong: the threshold had been calibrated on an F32-activation runtime.

**Production path:**
- **Templates:** llama-server's own `--jinja` rendering (`/apply-template`) is byte-identical to Transformers' `apply_chat_template` for single-turn, system + user, multi-turn and `enable_thinking=false` conversations. It **differs** when the history contains an earlier assistant turn with `reasoning_content`: llama.cpp keeps the old reasoning that the official template drops. The adapter must strip reasoning from the history it sends.
- **Tokenization:** llama-server's tokenizer is identical to the reference on a 9,357-token document and on emoji, ZWJ sequences, CJK, combining marks, Arabic, Hebrew, Hindi, CRLF and long runs of the same character.

**Acceptance gate for future updates.** It has two parts, and both must pass:
1. **Precision:** `gguf_identity.py` reports IDENTICAL, the SHA-256 matches, and the launch flags include `-fa off -ctk bf16 -ctv bf16`. This is the gate that actually detects quantization.
2. **Correctness (llama.cpp configuration only):** `lfm3_check.py all` plus `lfm3_redteam.py longctx`. Every short-prompt gap is < 0.05, every long-context gap is < 0.12, and the reference's top-10 tokens are always present. These thresholds sit above our worst llama.cpp BF16 result (0.026 short, 0.082 long) and below Q8_0's (≥ 0.075 short, 0.42 long). They apply **only to an F32-activation runtime compared with the F32 reference.** A BF16-compute runtime such as MLX must instead be compared with the BF16 reference (`lfm3_redteam.py longctx_ref_bf16` then `bf16_compare`) and judged against the ~0.4 spread that BF16 arithmetic produces by itself.

## Performance (llama.cpp, BF16 KV, M4)

`llama-bench`, 2 runs each. These were measured while no other model process was running; the first report's numbers came from a run that overlapped another process and read ~7% low.

| Context already in memory | `-fa off`: prompt / generation | `-fa on`: prompt / generation |
|---|---|---|
| 0 | 946 / **29.1** tokens/s | 954 / 29.1 tokens/s |
| 8K | 716 / **26.7** tokens/s | 797 / 28.5 tokens/s |

Generation at 29 tokens/s is ~85% of the M4's memory-bandwidth limit (3.46 GB of weights per token, ~120 GB/s).

**Prompt reuse is partial.** Because the model has a recurrent part and a sliding window, llama.cpp can only resume from a saved snapshot and recomputes everything after it. Repeating a 6,390-token prompt still re-processed 516 tokens, and an edit early in a long chapter means re-processing almost all of it. The chat design should keep stable content (system prompt, attached documents) at the front and volatile content at the end.

## Choosing between llama.cpp and MLX

Both are correct, unquantized BF16-weight runtimes. They differ in compute precision and serving:

| | llama.cpp (`-fa off`) | mlx-vlm |
|---|---|---|
| Compute precision | BF16 weights; BF16×BF16 prompt matmuls; F32 activations, norms and softmax | BF16 throughout (the most literal reading of "BF16 computation") |
| Distance from full-precision math | 0.08 (closer) | 0.44 (typical of BF16 compute) |
| Generation speed | ~29 tokens/s | ~25 tokens/s |
| Serving | Mature `llama-server`: official Jinja template, slots, Unix socket, API key | mlx-lm can't load LFM3 yet. mlx-vlm is a vision-language package used here for text; its server has **not been evaluated** |
| Quirks found | F16 in flash attention (turned off) | None found |

**We chose llama.cpp** for its maturity in serving and its closeness to full-precision results, not because MLX is incorrect. If the spec is read as requiring BF16 arithmetic everywhere, MLX qualifies, but its serving path would need the same evaluation llama.cpp got: templates, concurrency, socket, injection.

## Chat and observer together

With 2 slots, one server and **no scheduling**, an observer request with a 4,096-token prompt **stalls a streaming chat reply for up to 2.2 s**. Normal token gaps are ~34 ms. The independent review saw a 7.5 s delay before a chat reply's first token, behind an 11K-token observer job. llama-server has no slot priority.

| Logical batch (`-b`/`-ub`) | Worst chat stall | Observer's 4K-token prompt |
|---|---|---|
| 2048 (default) | 2,232 ms | 4.7 s |
| 512 | 637 ms | 5.1 s |
| 128 | 195 ms | 6.1 s |

The micro-batch alone (`-ub`) makes no difference; the logical batch (`-b`) is what sets the interleaving. **The required design** is that the app's scheduler runs observer jobs only while no chat request is active, in chunks, and cancels them when the author sends a message. `-b 256` is a backstop, not the solution. Its stall is *interpolated* at ~300–400 ms (128 and 512 were measured; 256 was not). Note also that `-c` is split **evenly** across slots: `-np 2 -c 131072` gives each slot 64K. Unequal context sizes need two server processes.

## Memory

Recommended flags (`-fa off`, `-np 2`, `--cache-ram 0`), with both slots holding an 8.5K-token prompt. The figures are system-level: the drop in free memory with the weights already in the page cache. The process "footprint" figure misses GPU-private buffers, so it isn't used here.

| Configuration | KV cache | GPU compute buffer | Server memory beyond the 3.8 GB of weights |
|---|---|---|---|
| `-c 131072` (64K × 2), `-b/-ub 512` | 1.55 GB | 2.20 GB | **4.1 GB** |
| `-c 131072` (64K × 2), `-b/-ub 128` | 1.55 GB | 0.55 GB | **3.05 GB** |

**With flash attention off, the compute buffer scales with micro-batch × per-slot context.** At full 256K context with `-ub 512` it would be ~8.8 GB, so the pure-BF16 path caps practical context on a 16 GB machine. Free memory fell to 0.08 GB during the `-ub 512` run, because this Mac had only ~4.2 GB free beforehand. **Proposed defaults:** 32K per slot (`-np 2 -c 65536 -b/-ub 256`). That was estimated at 3.8 GB of weights plus ~1.4 GB, and the measurements below confirm it. The settings panel caps context at 32K per conversation.

**Measured 2026-11-04** (llama-server's own memory breakdown, `-np 2 -b/-ub 256`, BF16 KV, `-fa off`):

| `-c` (total) | KV | Metal compute | CPU compute |
|---|---|---|---|
| 4096 | 58 MiB | 47 MiB | 25 MiB |
| 16384 | 203 MiB | 149 MiB | 37 MiB |
| 32768 | 395 MiB | 285 MiB | 53 MiB |
| 65536 (default, 32K × 2) | 779 MiB | 557 MiB | 85 MiB |

That works out to about 21.5 KiB per token of `-c`: 12 KiB of KV (6 attention layers), 8.5 KiB of Metal compute and 1 KiB of CPU compute. Weights are 3.3 GB Metal-mapped plus 0.5 GB CPU-mapped from a 3.47 GB file. The embedding server (Qwen3-Embedding-0.6B, fixed at 2,304 tokens) needs about 2.2 GB in all.

**Memory check before starting (`src/llm/memfit.c`).** The estimate is the model file, plus a fixed cost, plus 22 KiB per token (`llm_server_mem_need`). This is compared with what macOS can hand out, keeping 1 GiB of headroom. That's the total minus wired, compressed and apps' own anonymous memory, as Activity Monitor counts it; cached files count as available. Free + inactive alone read 7.1 GB on a machine with 8.9 GB to give. If the author's context doesn't fit, the model starts with the largest context, in 8K steps, that does, and the status bar says so. The saved setting is kept, and Apply in Model settings checks again. If not even 8K fits, the model starts anyway with a warning. The same check runs before a restart for a new context size, counting the running model's memory as returned. The Model settings panel shows the estimate for the context being chosen against what's free.

The server's own defaults are **4 slots and an 8 GB host prompt cache**, so `-np` and `--cache-ram` must always be set explicitly.

## Deployment security

- **No TCP port:** `--host <path>.sock` opens no TCP listener, and requests without the API key get 401 (independent review).
- **Socket permissions:** the socket file is created `srwxr-xr-x`, so protection rests on its directory being `0700`.
- **Stale sockets:** after a crash or SIGTERM, the `.sock` file stays behind and a restart fails with `couldn't bind HTTP server socket`. The supervisor must remove it before every launch.
- **Prompt injection through special tokens.** Literal `<|im_start|>` / `<|im_end|>` text in a message becomes real control tokens: an injected message produced 4 `im_start` and 3 `im_end` tokens, against 2 and 1 for clean text, in both llama-server and Transformers. A pasted or imported manuscript could forge a system turn. The adapter must neutralize special-token strings in all user and document content, for example by inserting U+200C ZERO WIDTH NON-JOINER after `<|`, and this needs a test.

## License (not legal advice)

The model ships under the LFM Open License v1.0, which is Apache-2.0-style:
- **Redistribution is permitted,** with the license, notices and modification markings included (§4).
- **Commercial use is licensed only for an entity with less than $10M in annual revenue** (§5). This applies to *use*, so it covers Scribodo's users and not just its distribution. Downloading the weights at first run does **not** avoid it.

The first version of this report got this wrong. The question needs a legal review before anything ships.

## Open items

- **Scope:** the MVP targets this Mac only (Apple M4, 16 GB, macOS 26). Linux was dropped by decision on 2026-11-04, overriding SOW §6, so Linux runtime paths won't be validated.
- **Context fill:** 256K was never tested with the cache actually filled.
- **The `enable_thinking=false` default:** correctness for prose-style replies was checked only by the reviewer; add it to `lfm3_check.py`.
- **Re-run on update:** re-run both gates on every llama.cpp or model update. LFM3 support is days old on `master`.

## Red-team review

There were two passes: the author's own, and an independent reviewer that was given the code and the results but not the author's concerns. The reviewer reproduced 7 of its 10 findings. Status of each: **F** fixed or verified in this revision, **D** documented as a constraint for the chat design, **O** still open.

| # | Finding | Source | Status |
|---|---|---|---|
| 1 | Test prompts were ≤ 86 tokens; the sliding window and long context were never tested | both | **F**: long-context check to 4K; llama.cpp passes |
| 2 | **Flash attention stages values in F16** (decode Q/K/V; prefill output accumulator); "never narrower" was false | reviewer (source read), confirmed by author | **F**: `-fa off` recommended; kernel census verified |
| 3 | **Output-closeness gate can't detect 8-bit quantization**; Q8_0 passed it | author | **F**: precision gate is now structural (`gguf_identity.py` rejects Q8_0) |
| 4 | mlx-vlm drifts 0.44 from the F32 reference at the window boundary and on ~1.9K chat prompts | both | **Retracted on follow-up**: a BF16 reference drifts the same (0.40); this is BF16 arithmetic, not an MLX defect. MLX is a correct runtime; llama.cpp chosen for serving |
| 5 | The observer stalls chat (2.2 s; 7.5 s to first token); "can be deprioritized" was unsupported; `-c` splits evenly | both | **D**: app-level scheduler required; `-b 256` backstop |
| 6 | Special-token text in content becomes control tokens | both | **D**: adapter must neutralize; test required |
| 7 | llama.cpp keeps old `reasoning_content` that the official template drops | author | **D**: adapter strips history reasoning |
| 8 | Memory table built from footprint (missed GPU buffers) and default flags (4 slots, 8 GB host cache); FA-off compute buffer not counted | both | **F**: re-measured system-level with explicit flags |
| 9 | Prompt reuse is partial (snapshots), so "can reuse" overstated it | reviewer | **D**: put stable context first |
| 10 | Stale socket blocks restart; socket mode is 0755 | reviewer | **D**: supervisor removes the socket; 0700 directory |
| 11 | License misread: the $10M commercial-use threshold applies to use | both | **F**: corrected; legal review flagged |
| 12 | Provenance: logs overwritten between runs; results came from a different script version; GGUF revision in an untracked file; bit-identity check not in the repo | reviewer | **F**: per-stage logs, commit, flags and SHA-256 recorded in results; revision pinned in code; `gguf_identity.py` added |
| 13 | The metric took the candidate's top-10, not the reference's; top-1 failures are near-ties | both | **F**: reference top-10 with coverage check; near-ties analyzed |
| 14 | Startup "verify BF16 from the log" doesn't work at default verbosity | reviewer | **F**: verify the artifact hash and flags instead |
| 15 | Performance numbers came from a run that overlapped another process | author | **F**: re-benchmarked in isolation |
| 16 | Linux and a filled 256K context untested | reviewer | Linux out of scope (decision 2026-11-04); filled 256K context **O** |
