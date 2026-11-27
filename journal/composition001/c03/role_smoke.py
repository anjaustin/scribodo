#!/usr/bin/env python3
"""Benign synthetic role/endpoint smoke, serial authenticated children, no store or product data.

Criteria are frozen in this source: source-exact extraction (fresh/reused/across slots),
finite normalized 1024-D embeddings with the identical passage ranked first, and the
answer-bearing passage ranked first by the production reranker scoring rule.
Timings/RSS/differences are descriptive, not a new quality/resource acceptance budget.
"""
import argparse
from contextlib import contextmanager
import importlib.util
import json
import math
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from answer_common import UnixHTTP, private_socket, sha
from answer_unit_extraction import EMBED_GGUF, EMBED_SHA, QUERY_INSTRUCTION, EXTRACTOR, SCHEMAS, request
from answer_rerank_arm import rerank_prompt
spec = importlib.util.spec_from_file_location("c03_probe", Path(__file__).with_name("compatibility_probe.py"))
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)

MODELS = {
    "chat": (probe.GGUF, probe.GGUF_SHA),
    "embed": (EMBED_GGUF, EMBED_SHA),
    "rerank": (Path.home() / "Library/Application Support/Scribodo/models/Qwen3-Reranker-0.6B-bf16.gguf",
               "5a6bf98df1ba7b7a957bbcfc14e74a73b4c314812c2224fe048ea942db4e53f1"),
}
PASSAGE = "The observatory key is amber."
QUESTION = "What color is the observatory key?"
DOCUMENTS = ["Zoë keeps the amber observatory key.", "The harbor bell is made of iron.",
             "Emil studies ferns in the forest."]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


@contextmanager
def server(binary, role, out):
    model, expected = MODELS[role]
    require(model.is_file() and sha(model) == expected, f"{role}: unadmitted artifact")
    version = subprocess.check_output([str(binary), "--version"], stderr=subprocess.STDOUT, text=True, timeout=30)
    manifest = {"binary_sha256": sha(binary), "version": version, "model_sha256": expected,
                "source_sha256": sha(Path(__file__)), "role": role}
    with private_socket() as sock:
        key = secrets.token_hex(24)
        keyfile = sock.parent / "api-key"
        fd = os.open(keyfile, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, "w") as file:
            file.write(key + "\n")
        if role == "chat":
            flags = list(probe.FLAGS)
            flags[flags.index("-np") + 1] = "2"
            flags[flags.index("-c") + 1] = "16384"
            flags += ["--load-mode", "auto"]
        else:
            ctx = "2304" if role == "embed" else "2048"
            flags = ["-ngl", "99", "-fa", "off", "-np", "1", "-c", ctx, "-b", ctx, "-ub", ctx,
                     "--cache-ram", "0", "--no-webui", "--no-slots", "-v"]
            flags += ["--embedding", "--pooling", "last"] if role == "embed" else ["-ctk", "bf16", "-ctv", "bf16"]
        manifest["flags"] = flags
        argv = [str(binary), "-m", str(model), "--host", str(sock), "--api-key-file", str(keyfile), *flags]
        with (out / "server.log").open("x") as log:
            start = time.monotonic()
            process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 180
                while True:
                    require(process.poll() is None, f"{role}: server exited during startup")
                    try:
                        if probe.call(sock, key, "/health", timeout=1).get("status") == "ok":
                            break
                    except (OSError, RuntimeError):
                        pass
                    require(time.monotonic() < deadline, f"{role}: startup timeout")
                    time.sleep(0.2)
                manifest["startup_seconds"] = time.monotonic() - start
                rss = subprocess.check_output(["ps", "-o", "rss=", "-p", str(process.pid)], text=True).strip()
                manifest["post_startup_rss_kib_not_whole_system"] = int(rss)
                yield lambda endpoint, body: probe.call(sock, key, endpoint, body), manifest
            finally:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)


def run(binary, label, role, out):
    out.mkdir()
    rows = []
    with server(binary, role, out) as (call, manifest):
        require(probe.COMMITS[label] in manifest["version"], "Mislabeled binary")
        def observe(endpoint, body):
            start = time.monotonic()
            reply = call(endpoint, body)
            rows.append({"endpoint": endpoint, "request": body, "reply": reply,
                         "seconds": time.monotonic() - start})
            return reply
        if role == "chat":
            messages = [{"role": "system", "content": EXTRACTOR}, {"role": "user", "content":
                        "Copy the sentence that answers the question exactly; return span only.\n"
                        f"Question: {QUESTION}\nPassage: {PASSAGE}"}]
            body = request(messages, SCHEMAS["span_uncapped"], "evidence", 128)
            spans = []
            for slot, cache in ((0, False), (0, True), (1, False), (0, True)):
                body = {**body, "id_slot": slot, "cache_prompt": cache}
                observe("/apply-template", body)
                reply = observe("/v1/chat/completions", body)
                choice = reply["choices"][0]
                content = json.loads(choice["message"]["content"])
                require(choice["finish_reason"] == "stop" and set(content) == {"span"} and
                        content["span"] == PASSAGE, "Schema/source-exact extraction or slot reuse failed")
                spans.append(content["span"])
            manifest["source_exact_extractions"] = len(spans)
        elif role == "embed":
            reply = observe("/v1/embeddings", {"input": [QUERY_INSTRUCTION + DOCUMENTS[0], *DOCUMENTS]})
            rows_by_index = {item["index"]: item["embedding"] for item in reply["data"]}
            require(set(rows_by_index) == {0, 1, 2, 3} and len(reply["data"]) == 4, "Embedding row loss")
            vectors = []
            for index in range(4):
                vector = rows_by_index[index]
                require(len(vector) == 1024 and all(math.isfinite(x) for x in vector), "Invalid embedding")
                norm = math.sqrt(sum(x * x for x in vector))
                require(norm > 0, "Zero embedding")
                vectors.append([x / norm for x in vector])
            scores = [sum(x * y for x, y in zip(vectors[0], vector)) for vector in vectors[1:]]
            require(max(range(3), key=scores.__getitem__) == 0, "Identical passage no longer ranked first")
            manifest["cosines"] = scores
        else:
            ids = [observe("/tokenize", {"content": word, "add_special": False})["tokens"] for word in ("yes", "no")]
            require(all(len(x) == 1 for x in ids), "Reranker decision token changed")
            scores = []
            for document in (PASSAGE, DOCUMENTS[1], DOCUMENTS[2]):
                reply = observe("/completion", {"prompt": rerank_prompt(QUESTION, document), "n_predict": 1,
                    "n_probs": 20, "temperature": 0, "top_k": 1, "samplers": ["top_k"],
                    "cache_prompt": False, "post_sampling_probs": False, "id_slot": 0})
                top = reply["completion_probabilities"][0]["top_logprobs"]
                values = {x["id"]: math.exp(x["logprob"]) for x in top}
                yes, no = (values.get(x[0], 0) for x in ids)
                require(yes + no > 0 and all(math.isfinite(x) for x in (yes, no)), "Invalid yes/no score")
                scores.append(yes / (yes + no))
            require(max(range(3), key=scores.__getitem__) == 0, "Reranker answer-bearing passage lost first place")
            manifest["scores"] = scores
    probe.save(out / "observations.json", {"manifest": manifest, "rows": rows})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--label", choices=probe.COMMITS, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    active = subprocess.run(["pgrep", "-x", "llama-server"], capture_output=True)
    require(active.returncode == 1 and not active.stdout, "Existing server present; leave untouched")
    args.out.mkdir()
    for role in MODELS:
        run(args.server.resolve(), args.label, role, args.out / role)
    print("PASS: source-exact schema/reuse, embedding and reranker synthetic role smoke", flush=True)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    main()
