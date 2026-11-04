#!/usr/bin/env python3
"""Red-team experiments for the LFM3 runtime decision (see docs/MODEL-001).

Each experiment targets a gap in lfm3_check.py:

  longctx      Next-token distributions at positions that cross the 256-token
               sliding window and extend to 4K tokens (the original check
               never exceeded ~86 tokens). Stages: longctx_ref, longctx_mlx,
               longctx_llama, then longctx_compare.
  template     The production path: llama-server's own Jinja rendering
               (/apply-template) vs Transformers' apply_chat_template, for
               multi-turn conversations with and without thinking.
  tokenize     llama-server's tokenizer vs the reference on a larger,
               adversarial corpus.
  concurrency  Chat streaming while an observer request prefills a long
               prompt in a second slot: does the observer stall the chat?

Run one at a time (memory): build/venv-model/bin/python tools/model_check/lfm3_redteam.py <name>
"""
import json
import math
import os
import socket
import statistics
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
import lfm3_check as base  # noqa: E402

OUT = base.OUT_DIR
POSITIONS = [200, 255, 256, 257, 300, 512, 1024, 2048, 3072, 4096]


def corpus_text():
    parts = []
    for p in ("docs/SOW.md", "README.md", "assets/sample/sample.md", "docs/REDTEAM-001-editor-spike.md"):
        with open(p, encoding="utf-8") as f:
            parts.append(f.read())
    return "\n\n".join(parts)


def long_ids(tok):
    ids = tok.encode(corpus_text(), add_special_tokens=True)
    if len(ids) < max(POSITIONS):
        raise SystemExit("corpus too short: %d tokens" % len(ids))
    return ids[: max(POSITIONS)]


# ---- llama-server helper ---------------------------------------------------

class Server:
    def __init__(self, extra=(), ctx=8192, slots=1, gguf_file=base.GGUF_FILE):
        from huggingface_hub import hf_hub_download
        self.gguf = hf_hub_download(base.GGUF_REPO, gguf_file, revision=base.GGUF_REV)
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        self.port = s.getsockname()[1]
        s.close()
        os.makedirs(OUT, exist_ok=True)
        self.log = open(os.path.join(OUT, "redteam-%s-server.log" % base.STAGE_NAME), "w")
        flags = [f for f in base.SERVER_FLAGS]
        flags[flags.index("-np") + 1] = str(slots)
        self.proc = subprocess.Popen(
            [base.LLAMA_SERVER, "-m", self.gguf, "--host", "127.0.0.1", "--port", str(self.port),
             "-c", str(ctx), *flags, *extra], stdout=self.log, stderr=self.log)
        self.url = "http://127.0.0.1:%d" % self.port

    def __enter__(self):
        import requests
        for _ in range(240):
            try:
                if requests.get(self.url + "/health", timeout=1).json().get("status") == "ok":
                    return self
            except Exception:
                pass
            time.sleep(0.5)
        raise RuntimeError("llama-server not ready")

    def __exit__(self, *a):
        self.proc.terminate()
        self.proc.wait(timeout=30)


# ---- long context ------------------------------------------------------------------

def longctx_ref(dtype_name="float32", out_name="longctx_ref"):
    import torch
    from transformers import AutoModelForCausalLM
    tok = base.tokenizer()
    ids = long_ids(tok)
    model = AutoModelForCausalLM.from_pretrained(base.REPO, revision=base.REV, trust_remote_code=True,
                                                 dtype=getattr(torch, dtype_name))
    model.eval()
    with torch.no_grad():
        h = model.model(torch.tensor([ids])).last_hidden_state[0]
        rows = torch.stack([h[p - 1] for p in POSITIONS])
        lp = torch.log_softmax(model.lm_head(rows).float(), -1)
    base.save(out_name, {"n": len(ids), "positions": POSITIONS, "logprobs": lp.tolist(), "dtype": dtype_name,
                         "attn_impl": getattr(model.config, "_attn_implementation", "?")})


def longctx_ref_bf16():
    """Same reference code, but computing in BF16: separates 'MLX is wrong'
    from 'MLX computes activations in BF16'."""
    longctx_ref("bfloat16", "longctx_ref_bf16")


def bf16_compare():
    """Distance of each runtime from the F32 and from the BF16 reference."""
    refs = {"F32 ref": base.load_json("longctx_ref"), "BF16 ref": base.load_json("longctx_ref_bf16")}
    mlx = base.load_json("longctx_mlx")
    lla = base.load_json("longctx_llama")

    def gaps_full(r, c):
        out = []
        for i in range(len(r["positions"])):
            a, b = r["logprobs"][i], c["logprobs"][i]
            top10 = sorted(range(len(a)), key=a.__getitem__, reverse=True)[:10]
            out.append(max(abs(a[t] - b[t]) for t in top10))
        return out

    def gaps_top(r, c):
        out = []
        for i in range(len(r["positions"])):
            a = r["logprobs"][i]
            cmap = {t["id"]: t["logprob"] for t in c["top"][i]}
            top10 = sorted(range(len(a)), key=a.__getitem__, reverse=True)[:10]
            out.append(max(abs(a[t] - cmap[t]) if t in cmap else float("inf") for t in top10))
        return out

    rows = [("mlx-vlm (BF16 activations)", lambda r: gaps_full(r, mlx)),
            ("llama.cpp -fa off (F32 activations)", lambda r: gaps_top(r, lla)),
            ("BF16 ref vs F32 ref", lambda r: gaps_full(refs["F32 ref"], refs["BF16 ref"]) if r is refs["F32 ref"] else None)]
    print("positions:", refs["F32 ref"]["positions"])
    for name, fn in rows:
        for rname, r in refs.items():
            g = fn(r)
            if g is None:
                continue
            print("%-38s vs %-8s worst %.3f  median %.3f  | %s" % (
                name, rname, max(g), sorted(g)[len(g) // 2], " ".join("%.3f" % x for x in g)))


def longctx_mlx():
    import mlx.core as mx
    from mlx_vlm import load
    tok = base.tokenizer()
    ids = long_ids(tok)
    model, _ = load(base.REPO, revision=base.REV)
    logits = model.language_model(mx.array([ids])).logits[0].astype(mx.float32)
    out = []
    for p in POSITIONS:
        row = logits[p - 1]
        out.append((row - mx.logsumexp(row)).tolist())
    base.save("longctx_mlx", {"positions": POSITIONS, "logprobs": out})


def longctx_llama():
    import requests
    tok = base.tokenizer()
    ids = long_ids(tok)
    out = []
    with Server(ctx=8192) as s:
        for p in POSITIONS:
            r = requests.post(s.url + "/completion", json={
                "prompt": ids[:p], "n_predict": 1, "temperature": 0.0, "top_k": 1, "samplers": ["top_k"],
                "n_probs": base.TOP_N, "post_sampling_probs": False, "cache_prompt": False}).json()
            top = r["completion_probabilities"][0]["top_logprobs"]
            out.append([{"id": t["id"], "logprob": t["logprob"]} for t in top])
    name = "longctx_llama" if base.STAGE_NAME == "llamacpp" else "longctx_" + base.STAGE_NAME
    base.save(name, {"positions": POSITIONS, "top": out, "gguf": base.GGUF_FILE, "fa": base.FA, **base.provenance()})


def longctx_compare():
    ref = base.load_json("longctx_ref")
    lla_name = os.environ.get("LFM3_LONGCTX_LLAMA", "longctx_llama")
    print("reference: %d tokens, attention implementation %s" % (ref["n"], ref["attn_impl"]))
    print("%6s | %-34s | %-34s" % ("pos", "mlx: top1  max|dlogp|10   KL", "llama.cpp: top1 max|dlogp|10 KL(top20)"))
    for name in ("longctx_mlx", "longctx_llama"):
        try:
            base.load_json(name)
        except FileNotFoundError:
            print("missing", name)
    mlx = base.load_json("longctx_mlx")
    lla = base.load_json(lla_name)
    print("llama.cpp column:", lla.get("gguf", base.GGUF_FILE), "flash attention", lla.get("fa", "?"))
    worst = {"mlx": 0.0, "llama": 0.0}
    for i, p in enumerate(ref["positions"]):
        r = ref["logprobs"][i]
        top1 = max(range(len(r)), key=r.__getitem__)
        top10 = sorted(range(len(r)), key=r.__getitem__, reverse=True)[:10]
        m = mlx["logprobs"][i]
        m_top1 = max(range(len(m)), key=m.__getitem__) == top1
        m_d = max(abs(r[t] - m[t]) for t in top10)
        m_kl = base.kl_full(r, m)
        lt = lla["top"][i]
        l_top1 = lt[0]["id"] == top1
        cand = {t["id"]: t["logprob"] for t in lt}
        l_d = max(abs(r[t] - cand[t]) if t in cand else float("inf") for t in top10)
        a = base.log_softmax([r[t["id"]] for t in lt])
        b = base.log_softmax([t["logprob"] for t in lt])
        l_kl = sum(math.exp(x) * (x - y) for x, y in zip(a, b))
        worst["mlx"] = max(worst["mlx"], m_d)
        worst["llama"] = max(worst["llama"], l_d)
        print("%6d | %-5s %8.3f %12.2e | %-5s %8.3f %12.2e" % (p, "same" if m_top1 else "DIFF", m_d, m_kl,
                                                            "same" if l_top1 else "DIFF", l_d, l_kl))
    print("worst max|dlogp| top10: mlx %.3f, llama.cpp %.3f" % (worst["mlx"], worst["llama"]))


# ---- production template path ------------------------------------------------------

CONVERSATIONS = [
    ("single user", [{"role": "user", "content": "Brainstorm a title for my novel."}], {}),
    ("system + user", [{"role": "system", "content": "You are a creative writing partner."},
                       {"role": "user", "content": "Describe the lighthouse."}], {}),
    ("multi-turn", [{"role": "user", "content": "Who is Zoë?"},
                    {"role": "assistant", "content": "Zoë Ångström is the keeper's granddaughter."},
                    {"role": "user", "content": "And Dvořák?"}], {}),
    ("thinking off", [{"role": "user", "content": "One line about fog."}], {"enable_thinking": False}),
    ("history with thinking", [{"role": "user", "content": "Hi"},
                               {"role": "assistant", "content": "Hello!", "reasoning_content": "greet back"},
                               {"role": "user", "content": "Continue"}], {}),
]


def template():
    import requests
    tok = base.tokenizer()
    bad = 0
    with Server(extra=["--jinja"]) as s:
        for name, msgs, kw in CONVERSATIONS:
            ref = tok.apply_chat_template(msgs, tokenize=False, add_generation_prompt=True, **kw)
            body = {"messages": msgs}
            if kw:
                body["chat_template_kwargs"] = kw
            got = requests.post(s.url + "/apply-template", json=body).json().get("prompt")
            same = got == ref
            bad += not same
            print("%-22s %s" % (name, "identical" if same else "DIFFERENT"))
            if not same:
                print("   ref: %r\n   got: %r" % (ref[-160:], (got or "")[-160:]))
    print("template mismatches: %d/%d" % (bad, len(CONVERSATIONS)))


# ---- tokenizer parity on a larger corpus -------------------------------------------

def tokenize():
    import requests
    tok = base.tokenizer()
    samples = [corpus_text()]
    samples += [
        "\r\n\r\n  \t\t tabs and CRLF \r\n",
        "Emoji: 😀👩‍👩‍👧‍👦🏳️‍🌈 and flags 🇳🇴🇨🇿",
        "CJK 灯台守の孫娘は霧の中で手紙を読んだ。한국어 문장. 中文句子。",
        "Combining: é ä ñ vs precomposed é ä ñ; zero-width​join‍.",
        "Arabic: المنارة في الضباب. Hebrew: המגדלור בערפל. Hindi: कोहरे में प्रकाशस्तंभ।",
        "Math ∑∫√∞ ≠ ≤ ≥; quotes „“”‘’«» ‹›; dashes ‐‑‒–—―; ellipsis …",
        "   leading and trailing spaces   ",
        "x" * 3000,
        "<|im_start|>user\nliteral special-token text<|im_end|>",
    ]
    diffs = 0
    with Server() as s:
        for i, text in enumerate(samples):
            ref = tok.encode(text, add_special_tokens=False)
            got = requests.post(s.url + "/tokenize", json={"content": text, "add_special": False,
                                                           "parse_special": False}).json()["tokens"]
            same = ref == got
            diffs += not same
            print("sample %d (%d tokens): %s" % (i, len(ref), "identical" if same else "DIFFERENT"))
            if not same:
                k = next((j for j, (a, b) in enumerate(zip(ref, got)) if a != b), min(len(ref), len(got)))
                print("   first difference at token %d: ref %s got %s" % (k, ref[k:k + 5], got[k:k + 5]))
    print("tokenizer mismatches: %d/%d" % (diffs, len(samples)))


# ---- chat vs observer concurrency --------------------------------------------------

def stream_chat(url, n_predict, gaps, done):
    import requests
    last = None
    with requests.post(url + "/completion", json={"prompt": "Write a long, slow paragraph about fog over a fjord.",
                                                  "n_predict": n_predict, "stream": True, "temperature": 0.0,
                                                  "cache_prompt": False, "id_slot": 0}, stream=True) as r:
        for line in r.iter_lines():
            if not line.startswith(b"data: "):
                continue
            now = time.perf_counter()
            if last is not None:
                gaps.append(now - last)
            last = now
            if json.loads(line[6:]).get("stop"):
                break
    done.set()


def concurrency():
    import requests
    tok = base.tokenizer()
    ids = long_ids(tok)
    extra = os.environ.get("LFM3_EXTRA", "").split()
    print("server extra flags:", extra or "(none)", "| flash attention", base.FA)
    with Server(ctx=16384, slots=2, extra=extra) as s:
        base_gaps = []
        stream_chat(s.url, 120, base_gaps, threading.Event())
        gaps = []
        done = threading.Event()
        t = threading.Thread(target=stream_chat, args=(s.url, 200, gaps, done))
        t.start()
        time.sleep(1.0)  # chat is streaming; now the observer arrives with a long prompt
        t0 = time.perf_counter()
        requests.post(s.url + "/completion", json={"prompt": ids, "n_predict": 1, "cache_prompt": False,
                                                   "id_slot": 1})
        obs_s = time.perf_counter() - t0
        t.join()

    def summary(g):
        g = sorted(g)
        return "median %.0f ms, p95 %.0f ms, max %.0f ms" % (
            1000 * statistics.median(g), 1000 * g[int(0.95 * (len(g) - 1))], 1000 * g[-1])
    print("chat alone:              ", summary(base_gaps))
    print("chat + observer prefill:  ", summary(gaps), "| observer %d tokens took %.1f s" % (len(ids), obs_s))


def main():
    fns = {f.__name__: f for f in (longctx_ref, longctx_ref_bf16, longctx_mlx, longctx_llama, longctx_compare,
                                   bf16_compare, template, tokenize, concurrency)}
    name = sys.argv[1]
    if name == "longctx":
        for st in ("longctx_ref", "longctx_mlx", "longctx_llama"):
            print("== stage", st, flush=True)
            rc = subprocess.call([sys.executable, __file__, st])
            if rc:
                sys.exit(rc)
        longctx_compare()
    else:
        fns[name]()


if __name__ == "__main__":
    main()
