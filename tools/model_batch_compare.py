#!/usr/bin/env python3
"""The chat server's micro-batch measured: memory, speed and whether the replies change.

usage: tools/model_batch_compare.py OUT_DIR [UB ...]   (default UBs: 256 128 256, the last a control)

For each micro-batch the pinned llama-server starts with the app's chat arguments (two slots at 32K, flash attention
off, BF16 K and V, -b = -ub) on a loopback port, and:
  1. its own memory breakdown is read from its log (model, KV, Metal and CPU compute);
  2. prefill time is measured on prompts cut from the first novel's text at about 2,000, 5,000, 13,000 and 26,000
     tokens, cache off, three repeats, medians; generation speed on 128 tokens, three repeats, median;
  3. twenty captured composer requests (build/window/capture-pp and build/answer-pipe-capture/new6, the app's own
     request bodies) are replayed with their own sampling settings and seed, the reply cut at 400 tokens, and kept.
Then the replies are compared byte for byte between every pair of runs. A repeated micro-batch is the control: if
two runs at the same setting differ, a difference between settings says nothing.
Writes OUT_DIR/ub<N>-<run>/{server.log,breakdown.json,timings.json,replies.json} and OUT_DIR/summary.txt.
(REDTEAM-023: the first measurement of 2026-11-14 kept neither its script nor its logs, measured generation once,
and asserted the replies unchanged without replaying any.)"""
import glob, json, os, re, secrets, statistics, subprocess, sys, time, urllib.request
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL = glob.glob(os.path.expanduser("~/.cache/huggingface/hub/models--LiquidAI--LFM3-1.7B-GGUF/snapshots/*/LFM3-1.7B-BF16.gguf"))
MODEL = MODEL[0] if MODEL else os.path.expanduser("~/Library/Application Support/Scribodo/models/LFM3-1.7B-BF16.gguf")
PORT = 18097
KEY = secrets.token_hex(24)  # the loopback server serves captured manuscript requests: an API key, as the app uses
CAPTURES = sorted(glob.glob(f"{ROOT}/build/window/capture-pp/request-*.json")) + sorted(glob.glob(f"{ROOT}/build/answer-pipe-capture/new6/request-*.json"))
TEXT = "".join(open(p, encoding="utf-8").read() for p in sorted(glob.glob(f"{ROOT}/build/manuscripts/pride-and-prejudice/parts/part0[1-6].md")))


def post(path, body, timeout=900):
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json", "Authorization": f"Bearer {KEY}"})
    return json.load(urllib.request.urlopen(req, timeout=timeout))


def start(ub, log):
    p = subprocess.Popen([f"{ROOT}/build/llama/bin/llama-server", "-m", MODEL, "--host", "127.0.0.1", "--port", str(PORT),
                          "-ngl", "99", "-fa", "off", "-ctk", "bf16", "-ctv", "bf16", "--jinja", "--reasoning-format", "deepseek",
                          "-np", "2", "-c", "65536", "-b", str(ub), "-ub", str(ub), "--cache-ram", "0", "--no-webui", "--no-slots",
                          "--verbose"], stdout=log, stderr=log, env={**os.environ, "LLAMA_API_KEY": KEY})
    for _ in range(180):
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{PORT}/health", timeout=2)
            return p
        except Exception:
            time.sleep(1)
    p.kill()
    raise SystemExit("server did not start")


def breakdown(logpath):
    t = open(logpath, encoding="utf-8", errors="replace").read()
    out = {}
    m = re.findall(r"MTL0 \(Apple M4\)\s*\|\s*\d+\s*=\s*\d+\s*\+\s*\((\d+)\s*=\s*(\d+)\s*\+\s*(\d+)\s*\+\s*(\d+)\)", t)
    if m:
        out["device_total"], out["model"], out["kv"], out["metal_compute"] = map(int, m[-1])
    h = re.findall(r"- Host\s*\|\s*\d+\s*=\s*\d+\s*\+\s*\d+\s*\+\s*(\d+)", t)  # the host line's compute (both CPU buffers)
    if h:
        out["cpu_compute"] = int(h[-1])
    return out


def prompt_of(tokens_wanted):
    lo, hi = 1000, len(TEXT)
    while hi - lo > 200:
        mid = (lo + hi) // 2
        if len(post("/tokenize", {"content": TEXT[:mid]})["tokens"]) < tokens_wanted:
            lo = mid
        else:
            hi = mid
    return TEXT[:lo]


def run(ub, outdir):
    os.makedirs(outdir, exist_ok=False)
    with open(f"{outdir}/server.log", "w") as log:
        srv = start(ub, log)
        try:
            timings = {"prefill": {}, "generation": None}
            for t in (2000, 5000, 13000, 26000):
                pr = prompt_of(t)
                ms, n = [], 0
                for _ in range(3):
                    r = post("/completion", {"prompt": pr, "n_predict": 1, "cache_prompt": False, "temperature": 0})
                    ms.append(r["timings"]["prompt_ms"]); n = r["timings"]["prompt_n"]
                timings["prefill"][str(n)] = {"median_s": statistics.median(ms) / 1000, "runs_s": [x / 1000 for x in ms]}
            pr = prompt_of(2000)
            gen = [post("/completion", {"prompt": pr, "n_predict": 128, "cache_prompt": False, "temperature": 0})["timings"]["predicted_per_second"] for _ in range(3)]
            timings["generation"] = {"median_tps": statistics.median(gen), "runs_tps": gen}
            replies = {}
            for path in CAPTURES:
                body = json.load(open(path))
                body.pop("id_slot", None)
                body["stream"] = False
                body["max_tokens"] = min(int(body.get("max_tokens") or 400), 400)
                r = post("/v1/chat/completions", body)
                replies[os.path.relpath(path, ROOT)] = r["choices"][0]["message"].get("content") or ""
        finally:
            srv.terminate(); srv.wait()
    json.dump(breakdown(f"{outdir}/server.log"), open(f"{outdir}/breakdown.json", "w"), indent=1)
    json.dump(timings, open(f"{outdir}/timings.json", "w"), indent=1)
    json.dump(replies, open(f"{outdir}/replies.json", "w"), indent=1, ensure_ascii=False)
    return timings, replies


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr); return 1
    out = sys.argv[1]
    ubs = [int(x) for x in sys.argv[2:]] or [256, 128, 256]
    os.makedirs(out, exist_ok=True)
    runs, lines = [], []
    seen = {}
    for ub in ubs:
        seen[ub] = seen.get(ub, 0) + 1
        name = f"ub{ub}-{seen[ub]}"
        timings, replies = run(ub, f"{out}/{name}")
        runs.append((name, replies))
        b = json.load(open(f"{out}/{name}/breakdown.json"))
        pre = "  ".join(f"{k} tok {v['median_s']:.2f} s" for k, v in timings["prefill"].items())
        lines.append(f"{name}: memory {b}  prefill {pre}  generation {timings['generation']['median_tps']:.1f} tok/s")
        print(lines[-1], flush=True)
    for i in range(len(runs)):
        for j in range(i + 1, len(runs)):
            a, b = runs[i], runs[j]
            same = sum(a[1][k] == b[1].get(k) for k in a[1])
            lines.append(f"{a[0]} vs {b[0]}: {same} of {len(a[1])} replies byte-identical")
            print(lines[-1], flush=True)
    open(f"{out}/summary.txt", "w").write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
