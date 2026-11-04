#!/usr/bin/env python3
"""LFM3-1.7B BF16 runtime check for Scribodo (SOW feasibility gate, item 2).

Compares two candidate local runtimes against a reference implementation:

  reference  Hugging Face Transformers + Liquid's own modeling_lfm3.py, on CPU,
             with the official BF16 weights widened (exactly) to F32.
  mlx        mlx-vlm on the official BF16 safetensors (Metal GPU).
  llamacpp   llama.cpp llama-server on the official BF16 GGUF (Metal GPU),
             BF16 KV cache, flash attention off by default (LFM3_FA=on to
             compare; Metal flash attention stages values in F16).

Output closeness verifies *correctness*, not precision: an 8-bit model passes
these tolerances. Precision is verified structurally by gguf_identity.py plus
the launch flags (see docs/MODEL-001).

For each prompt it records the token ids, the full next-token distribution at
the first generated position, and a greedy continuation. The comparison then
reports tokenisation agreement, top-1 agreement, KL divergence from the
reference, and how many greedy tokens match before the first divergence.

Each runtime runs in its own process (stage) so their memory never overlaps.

Usage (from the repo root, with the model-check venv):
  build/venv-model/bin/python tools/model_check/lfm3_check.py all
  build/venv-model/bin/python tools/model_check/lfm3_check.py compare
"""
import json
import math
import os
import socket
import subprocess
import sys
import time

REPO = "LiquidAI/LFM3-1.7B"
REV = "0dc4268b561499c648c7ba7c9466bf3d8d01517a"
GGUF_REPO = "LiquidAI/LFM3-1.7B-GGUF"
GGUF_REV = "cd0c74c575b5c416f3011b5b4c298077f26ca8c2"
# Flash attention on Metal stages Q/K/V (decode) and the output accumulator
# (prefill) in F16, so the BF16-compliant configuration runs with it off.
# Override with LFM3_FA=on to measure the difference.
FA = os.environ.get("LFM3_FA", "off")
SERVER_FLAGS = ["-ngl", "99", "-fa", FA, "-ctk", "bf16", "-ctv", "bf16", "--no-warmup",
                "-np", "1", "--cache-ram", "0"]
GGUF_FILE = os.environ.get("LFM3_GGUF_FILE", "LFM3-1.7B-BF16.gguf")
# Negative control: LFM3_GGUF_FILE=LFM3-1.7B-Q4_K_M.gguf LFM3_STAGE_NAME=control_q4 runs the
# same llama.cpp stage on a quantized file, to show the comparison can detect it.
STAGE_NAME = os.environ.get("LFM3_STAGE_NAME", "llamacpp")
OUT_DIR = os.path.join("build", "model_check")
LLAMA_SERVER = os.path.join("build", "llama.cpp", "build", "bin", "llama-server")
N_GEN = 40
TOP_N = 20

CHAT_PROMPTS = [
    "Write one vivid sentence about a lighthouse keeper in fog.",
    "Zoë Ångström is a lighthouse keeper’s granddaughter. Suggest three titles for a novel about her.",
    "Schreibe einen kurzen Satz über einen Leuchtturm im Nebel.",
]
RAW_PROMPT = (
    "The fog came in off the fjord the way it always did in late October: quietly, "
    "completely, and without asking. Zoë Ångström stood at the top of the lighthouse "
    "stairs with a lantern in one hand and"
)


def tokenizer():
    from transformers import AutoTokenizer
    return AutoTokenizer.from_pretrained(REPO, revision=REV, trust_remote_code=True)


def prompt_texts(tok):
    texts = []
    for p in CHAT_PROMPTS:
        texts.append(tok.apply_chat_template([{"role": "user", "content": p}],
                                             tokenize=False, add_generation_prompt=True))
    texts.append(RAW_PROMPT)
    return texts


def prompt_ids(tok):
    # Chat texts already contain the BOS/special tokens from the template.
    out = []
    for i, t in enumerate(prompt_texts(tok)):
        out.append(tok.encode(t, add_special_tokens=(i == len(CHAT_PROMPTS))))
    return out


def log_softmax(xs):
    m = max(xs)
    s = math.log(sum(math.exp(x - m) for x in xs)) + m
    return [x - s for x in xs]


def save(name, data):
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, name + ".json"), "w") as f:
        json.dump(data, f)


def load_json(name):
    with open(os.path.join(OUT_DIR, name + ".json")) as f:
        return json.load(f)


# ---- stages ---------------------------------------------------------------------

def stage_reference():
    import torch
    from transformers import AutoModelForCausalLM
    tok = tokenizer()
    t0 = time.time()
    model = AutoModelForCausalLM.from_pretrained(REPO, revision=REV, trust_remote_code=True,
                                                 dtype=torch.float32)
    model.eval()
    dtypes = sorted({str(p.dtype) for p in model.parameters()})
    results = {"runtime": "reference (transformers %s, torch %s, CPU, F32 compute from BF16 weights)"
               % (__import__("transformers").__version__, torch.__version__), **provenance(),
               "load_s": round(time.time() - t0, 1), "param_dtypes": dtypes, "prompts": []}
    with torch.no_grad():
        for ids in prompt_ids(tok):
            x = torch.tensor([ids])
            logits = model(x).logits[0, -1].float()
            lp = torch.log_softmax(logits, -1)
            gen = model.generate(x, max_new_tokens=N_GEN, do_sample=False, repetition_penalty=1.0,
                                 temperature=None, top_k=None, top_p=None)[0, len(ids):].tolist()
            results["prompts"].append({"ids": ids, "logprobs": lp.tolist(), "greedy": gen,
                                       "text": tok.decode(gen)})
    save("reference", results)


def stage_mlx():
    import mlx.core as mx
    import mlx_vlm
    from mlx.utils import tree_flatten
    from mlx_vlm import load
    tok = tokenizer()
    t0 = time.time()
    model, _ = load(REPO, revision=REV)
    params = tree_flatten(model.parameters())
    lm = model.language_model
    results = {"runtime": "mlx-vlm %s, mlx %s, Metal" % (mlx_vlm.__version__, mx.__version__),
               "load_s": round(time.time() - t0, 1),
               "param_dtypes": sorted({str(v.dtype) for _, v in params}),
               "quant_artifacts": sum(1 for k, _ in params if k.endswith(".scales")),
               "active_gb": round(mx.get_active_memory() / 1e9, 2), "prompts": []}
    for ids in prompt_ids(tok):
        seq = list(ids)
        first = None
        for _ in range(N_GEN):
            logits = lm(mx.array([seq])).logits[0, -1].astype(mx.float32)
            if first is None:
                first = (logits - mx.logsumexp(logits)).tolist()
            seq.append(int(mx.argmax(logits).item()))
        gen = seq[len(ids):]
        results["prompts"].append({"ids": ids, "logprobs": first, "greedy": gen, "text": tok.decode(gen)})
    results["peak_gb"] = round(mx.get_peak_memory() / 1e9, 2)
    save("mlx", results)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def provenance():
    """Who/what produced a result, so stale or mixed results are detectable."""
    try:
        commit = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], text=True).strip()
        dirty = bool(subprocess.check_output(["git", "status", "--porcelain", "tools/model_check"], text=True).strip())
    except Exception:
        commit, dirty = "?", True
    return {"script_commit": commit, "script_dirty": dirty, "time": time.strftime("%Y-%m-%dT%H:%M:%S")}


def sha256_file(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def stage_llamacpp():
    import requests
    from huggingface_hub import hf_hub_download
    sha = GGUF_REV
    gguf = hf_hub_download(GGUF_REPO, GGUF_FILE, revision=sha)
    tok = tokenizer()
    port = free_port()
    os.makedirs(OUT_DIR, exist_ok=True)
    log = open(os.path.join(OUT_DIR, "%s-server.log" % STAGE_NAME), "w")
    flags = SERVER_FLAGS + ["-c", "8192"]
    srv = subprocess.Popen([LLAMA_SERVER, "-m", gguf, "--host", "127.0.0.1", "--port", str(port), "-v"] + flags,
                           stdout=log, stderr=log)
    base = "http://127.0.0.1:%d" % port
    try:
        for _ in range(120):
            try:
                if requests.get(base + "/health", timeout=1).json().get("status") == "ok":
                    break
            except Exception:
                pass
            time.sleep(0.5)
        else:
            raise RuntimeError("llama-server did not become ready")
        results = {"runtime": "llama.cpp llama-server (Metal, %s, BF16 KV, flash attention %s)" % (GGUF_FILE, FA),
                   "gguf_revision": sha, "gguf_sha256": sha256_file(gguf), "server_flags": flags,
                   **provenance(), "prompts": []}
        for text, ids in zip(prompt_texts(tok), prompt_ids(tok)):
            own = requests.post(base + "/tokenize", json={"content": text, "add_special": False,
                                                          "parse_special": True}).json()["tokens"]
            r = requests.post(base + "/completion", json={
                "prompt": ids, "n_predict": N_GEN, "temperature": 0.0, "top_k": 1,
                "repeat_penalty": 1.0, "n_probs": TOP_N, "post_sampling_probs": False,
                "cache_prompt": False, "return_tokens": True, "samplers": ["top_k"]}).json()
            gen = r.get("tokens") or []
            first = r["completion_probabilities"][0]
            top = [{"id": t["id"], "logprob": t["logprob"]} for t in first["top_logprobs"]]
            results["prompts"].append({"ids": ids, "own_tokenization": own, "top": top, "greedy": gen,
                                       "text": tok.decode(gen), "timings": r.get("timings")})
        save(STAGE_NAME, results)
    finally:
        srv.terminate()
        srv.wait(timeout=30)


# ---- comparison -----------------------------------------------------------------

def prefix_match(a, b):
    n = 0
    for x, y in zip(a, b):
        if x != y:
            break
        n += 1
    return n


def kl_full(ref_lp, cand_lp):
    return sum(math.exp(r) * (r - c) for r, c in zip(ref_lp, cand_lp) if r > -30)


def compare():
    ref = load_json("reference")
    rows = []
    report = {"reference": ref["runtime"], "candidates": {}}
    for name in ("mlx", "llamacpp", "llamacpp_fa_on", "control_q4", "control_q8"):
        try:
            cand = load_json(name)
        except FileNotFoundError:
            continue
        per = []
        for i, (rp, cp) in enumerate(zip(ref["prompts"], cand["prompts"])):
            ref_lp = rp["logprobs"]
            ref_top1 = max(range(len(ref_lp)), key=ref_lp.__getitem__)
            entry = {"prompt": i, "greedy_prefix": prefix_match(rp["greedy"], cp["greedy"]),
                     "greedy_len": len(rp["greedy"])}
            if name == "mlx":  # full-vocabulary logits available
                c_lp = cp["logprobs"]
                entry["top1_match"] = max(range(len(c_lp)), key=c_lp.__getitem__) == ref_top1
                entry["kl"] = kl_full(ref_lp, c_lp)
                top = sorted(range(len(ref_lp)), key=ref_lp.__getitem__, reverse=True)[:10]
                entry["max_top10_dlogprob"] = max(abs(ref_lp[t] - c_lp[t]) for t in top)
            else:  # llama.cpp stages: top-N log-probabilities only
                entry["tokenization_match"] = cp["own_tokenization"] == rp["ids"]
                entry["top1_match"] = cp["top"][0]["id"] == ref_top1
                # KL restricted to the candidate's top-N (both renormalised there).
                ids = [t["id"] for t in cp["top"]]
                c = log_softmax([t["logprob"] for t in cp["top"]])
                r = log_softmax([ref_lp[t] for t in ids])
                entry["kl_topN"] = sum(math.exp(a) * (a - b) for a, b in zip(r, c))
                # Use the *reference's* top-10; each must appear in the candidate's top-N.
                cmap = {t["id"]: t["logprob"] for t in cp["top"]}
                ref_top10 = sorted(range(len(ref_lp)), key=ref_lp.__getitem__, reverse=True)[:10]
                entry["ref_top10_covered"] = all(t in cmap for t in ref_top10)
                entry["max_top10_dlogprob"] = max(abs(ref_lp[t] - cmap[t]) if t in cmap else float("inf")
                                                  for t in ref_top10)
                tm = cp.get("timings") or {}
                entry["gen_tps"] = round(tm.get("predicted_per_second", 0), 1)
            per.append(entry)
        report["candidates"][name] = {"runtime": cand["runtime"], "per_prompt": per,
                                      **{k: cand[k] for k in ("param_dtypes", "quant_artifacts", "active_gb", "peak_gb")
                                         if k in cand}}
    save("report", report)

    print("Reference:", ref["runtime"])
    for name, c in report["candidates"].items():
        print("\n%s  [%s]" % (name, c["runtime"]))
        for k in ("param_dtypes", "quant_artifacts", "active_gb", "peak_gb"):
            if k in c:
                print("  %s: %s" % (k, c[k]))
        for e in c["per_prompt"]:
            extra = ""
            if "kl" in e:
                extra = "KL %.2e" % e["kl"]
            if "kl_topN" in e:
                extra = "KL(top%d) %.2e  tokenization %s  %s tok/s" % (
                    TOP_N, e["kl_topN"], "same" if e["tokenization_match"] else "DIFFERENT", e["gen_tps"])
            print("  prompt %d: top-1 %s  greedy match %2d/%d  max|dlogp| top10 %.3f  %s" % (
                e["prompt"], "same" if e["top1_match"] else "DIFF", e["greedy_prefix"], e["greedy_len"],
                e["max_top10_dlogprob"], extra))
    print("\nReference greedy continuations:")
    for i, p in enumerate(ref["prompts"]):
        print("  %d: %r" % (i, p["text"][:110]))


def main():
    stage = sys.argv[1] if len(sys.argv) > 1 else "all"
    stages = {"reference": stage_reference, "mlx": stage_mlx, "llamacpp": stage_llamacpp,
              "llamacpp_fa_on": stage_llamacpp, "control_q4": stage_llamacpp, "control_q8": stage_llamacpp,
              "compare": compare}
    if stage == "all":
        for s in ("reference", "mlx", "llamacpp"):
            print("== stage", s, flush=True)
            rc = subprocess.call([sys.executable, __file__, s])
            if rc != 0:
                print("stage %s failed (rc=%d)" % (s, rc))
                sys.exit(rc)
        compare()
    else:
        stages[stage]()


if __name__ == "__main__":
    main()
