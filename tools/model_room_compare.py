#!/usr/bin/env python3
"""Room per token: the chat server's memory, cache reuse and output under alternative flags (2026-11-17).

usage: tools/model_room_compare.py OUT_DIR LABEL POOL TURNS DOC -- [llama-server flags ...]   (DOC 0, 1, or 2)
       tools/model_room_compare.py OUT_DIR --precision LABEL -- [flags ...]   (after a TURNS run labelled A0)
       tools/model_room_compare.py OUT_DIR --long LABEL N -- [flags ...]       (N prompts of about 25K tokens)
       tools/model_room_compare.py OUT_DIR --compare BASE LABEL ...

The pinned llama-server starts on a loopback port with the app's fixed chat arguments (flash attention off, BF16 K and
V, two slots, one pool -kvu, --cache-ram 0) plus the flags under test, with an API key as the app uses.
- A TURNS run plays a conversation of TURNS turns on slot 0, each a paragraph of the first novel and a question
  (DOC=1: an 8K-token chapter in the newest user message, before the words, as the app attaches documents; DOC=2:
  about 25K tokens of the novel instead, to fill most of a 40,960-cell pool), with a
  bounded side request on slot 1 between turns as the observer makes. It records the server's KV and compute buffers
  (its log at -lv 4) and, per turn, the prompt's tokens, those read again, and the time.
- A precision run takes the 25 turn prompts of the run labelled A0, generates 24 greedy tokens for each with the top 10
  log-probabilities, and --compare reports the largest top-10 probability difference over identical-context steps (up
  to the first differing token of each prompt) and every change of top token, with the base's top two there.
- A --long run gives N prompts of about 25,000 tokens (the novel's text from N offsets and a question) at a pool of
  40,960, greedy 24 tokens with the top 10, and records each prompt's read time; compared like a precision run.
- Every TURNS run also records what macOS can hand out (the app's own measure: total minus wired, compressed and apps'
  own memory) before the server starts, once it is ready, and after the last turn, so the estimate's fixed cost can
  be checked against what the server really takes (checkpoints and the process included).
Writes OUT_DIR/<label>.log and <label>.json (TURNS) or <label>-prec.json (precision, --long)."""
import glob, json, math, os, re, secrets, subprocess, sys, time, urllib.request
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = f"{ROOT}/build/llama/bin/llama-server"
MODEL = glob.glob(os.path.expanduser("~/.cache/huggingface/hub/models--LiquidAI--LFM3-1.7B-GGUF/snapshots/*/LFM3-1.7B-BF16.gguf"))
MODEL = MODEL[0] if MODEL else os.path.expanduser("~/Library/Application Support/Scribodo/models/LFM3-1.7B-BF16.gguf")
PARTS = sorted(glob.glob(f"{ROOT}/build/manuscripts/pride-and-prejudice/parts/part0[1-3].md")) or \
        sorted(glob.glob(f"{ROOT}/build/observer/base1-pp/part0[1-3].md"))
PORT = 18931
KEY = secrets.token_hex(24)
SYSTEM = "You are Scribodo, a writing assistant. Answer briefly and precisely about the novel the author is writing."


def available_mib():
    """What macOS can hand out, as src/llm/memfit.c counts it (sysmem_available)."""
    out = subprocess.run(["vm_stat"], capture_output=True, text=True).stdout
    page = int(re.search(r"page size of (\d+)", out).group(1))
    v = lambda k: int(re.search(rf"{k}:\s+(\d+)", out).group(1))
    total = int(subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True).stdout)
    used = v("Pages wired down") + v("Pages occupied by compressor") + max(0, v("Anonymous pages") - v("Pages purgeable"))
    return (total - used * page) / 2**20


def post(body):
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}/v1/chat/completions", json.dumps(body).encode(),
                                 {"Content-Type": "application/json", "Authorization": f"Bearer {KEY}"})
    with urllib.request.urlopen(req, timeout=900) as r:
        return json.load(r)


def start(out, label, pool, flags):
    keyfile = f"{out}/.{label}.key"
    with open(os.open(keyfile, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
        f.write(KEY)
    log = open(f"{out}/{label}.log", "w")
    args = [SERVER, "-m", MODEL, "--host", "127.0.0.1", "--port", str(PORT), "--api-key-file", keyfile, "-ngl", "99",
            "-fa", "off", "-ctk", "bf16", "-ctv", "bf16", "--jinja", "--reasoning-format", "deepseek", "-np", "2",
            "-c", str(pool), "-kvu", "--cache-ram", "0", "--no-webui", "--no-slots", "-lv", "4"] + flags
    proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(600):
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/health", timeout=2) as r:
                if r.status == 200:
                    return proc, log, keyfile
        except Exception:
            time.sleep(0.5)
    raise SystemExit("server did not start")


def book(big=False):
    paras = [p.strip() for p in open(PARTS[0]).read().split("\n\n") + open(PARTS[1]).read().split("\n\n") if len(p.strip()) > 200]
    if big:
        return paras, "".join(open(p).read() for p in sorted(glob.glob(os.path.dirname(PARTS[0]) + "/part0*.md")))[:110000]
    return paras, open(PARTS[2]).read()[:30000]


def words(paras, t):
    return f"Here is a passage:\n\n{paras[t % len(paras)]}\n\nWhat does it tell us about the characters?"


def turns(out, label, pool, n, with_doc, flags):
    paras, doc = book(with_doc == 2)
    avail = {"before": available_mib()}
    proc, log, keyfile = start(out, label, pool, flags)
    time.sleep(3)
    avail["ready"] = available_mib()
    rows, history = [], []
    try:
        for t in range(n):
            newest = (f"<document name=\"part03.md\">\n{doc}\n</document>\n\n" if with_doc else "") + words(paras, t)
            t0 = time.time()
            r = post({"messages": [{"role": "system", "content": SYSTEM}] + history + [{"role": "user", "content": newest}],
                      "id_slot": 0, "cache_prompt": True, "temperature": 0, "max_tokens": 160, "seed": 1,
                      "chat_template_kwargs": {"enable_thinking": False}})
            tm, reply = r.get("timings", {}), r["choices"][0]["message"]["content"]
            rows.append({"turn": t, "prompt_n": tm.get("prompt_n"), "cache_n": tm.get("cache_n"),
                         "prompt_ms": round(tm.get("prompt_ms", 0)), "wall_s": round(time.time() - t0, 2), "reply": reply})
            history += [{"role": "user", "content": words(paras, t)}, {"role": "assistant", "content": reply}]
            post({"messages": [{"role": "user", "content": "Name one character in: " + paras[(t + 7) % len(paras)][:1500]}],
                  "id_slot": 1, "cache_prompt": True, "temperature": 0, "max_tokens": 16, "seed": 1,
                  "chat_template_kwargs": {"enable_thinking": False}})
            print(f"{label} turn {t}: prompt {tm.get('prompt_n', 0) + tm.get('cache_n', 0)}, read {tm.get('prompt_n')} in "
                  f"{tm.get('prompt_ms', 0) / 1000:.2f} s", flush=True)
        time.sleep(3)
        avail["after"] = available_mib()
    finally:
        proc.terminate(); proc.wait(timeout=30); log.close(); os.unlink(keyfile)
    lines = [l.strip() for l in open(f"{out}/{label}.log", errors="replace")
             if re.search(r"(KV buffer size|RS buffer size|compute buffer size)", l)]
    mib = lambda key: sum(float(re.search(r"= *([\d.]+) MiB", l).group(1)) for l in lines if key in l)
    json.dump({"label": label, "pool": pool, "flags": flags, "kv_mib": mib("KV buffer") + mib("RS buffer"),
               "compute_mib": mib("compute buffer"), "available_mib": avail, "rows": rows, "memory_lines": lines},
              open(f"{out}/{label}.json", "w"), indent=1)


def precision(out, label, flags, long_n=0):
    paras, _ = book()
    prompts, history = [], []
    if long_n:
        text = "".join(open(p).read() for p in sorted(glob.glob(os.path.dirname(PARTS[0]) + "/part0*.md")))
        for k in range(long_n):
            start_at = k * 20000
            prompts.append([{"role": "system", "content": SYSTEM},
                            {"role": "user", "content": text[start_at:start_at + 110000] + "\n\nWho is mentioned last in this text, and what were they doing?"}])
    else:
        for t, row in enumerate(json.load(open(f"{out}/A0.json"))["rows"]):
            prompts.append([{"role": "system", "content": SYSTEM}] + history + [{"role": "user", "content": words(paras, t)}])
            history += [{"role": "user", "content": words(paras, t)}, {"role": "assistant", "content": row["reply"]}]
    proc, log, keyfile = start(out, label + "-prec", 40960, flags)
    result = []
    try:
        for p in prompts:
            r = post({"messages": p, "id_slot": 0, "cache_prompt": not long_n, "temperature": 0, "max_tokens": 24, "seed": 1,
                      "logprobs": True, "top_logprobs": 10, "chat_template_kwargs": {"enable_thinking": False}})
            result.append([{"tok": c["token"], "top": {x["token"]: math.exp(x["logprob"]) for x in c["top_logprobs"]},
                            "prompt_n": r.get("timings", {}).get("prompt_n"), "prompt_ms": r.get("timings", {}).get("prompt_ms")}
                           for c in r["choices"][0]["logprobs"]["content"]])
            if long_n:
                tm = r.get("timings", {})
                print(f"{label} long prompt: {tm.get('prompt_n')} tokens in {tm.get('prompt_ms', 0) / 1000:.1f} s", flush=True)
    finally:
        proc.terminate(); proc.wait(timeout=30); log.close(); os.unlink(keyfile)
    json.dump(result, open(f"{out}/{label}-prec.json", "w"))


def compare(out, base_label, labels):
    base = json.load(open(f"{out}/{base_label}-prec.json"))
    for label in labels:
        other = json.load(open(f"{out}/{label}-prec.json"))
        steps = flips = 0
        worst = 0.0
        for a, b in zip(base, other):
            for x, y in zip(a, b):
                steps += 1
                worst = max([worst] + [abs(x["top"].get(t, 0) - y["top"].get(t, 0)) for t in set(x["top"]) | set(y["top"])])
                if x["tok"] != y["tok"]:
                    flips += 1
                    top = sorted(x["top"].values(), reverse=True)[:2]
                    print(f"  {label}: top token {x['tok']!r} -> {y['tok']!r}, base top two {top[0]:.3f}/{top[1]:.3f}")
                    break
        print(f"{label}: {steps} identical-context steps, largest top-10 probability difference {worst:.4f}, top-token changes {flips}")


if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    flags = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if sys.argv[2] == "--precision":
        precision(out, sys.argv[3], flags)
    elif sys.argv[2] == "--long":
        precision(out, sys.argv[3], flags, int(sys.argv[4]))
    elif sys.argv[2] == "--compare":
        compare(out, sys.argv[3], sys.argv[4:])
    else:
        turns(out, sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), flags)
