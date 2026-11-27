#!/usr/bin/env python3
"""C03 compatibility checks; reuse MODEL-001 reference code and pinned artifacts.

Only legitimate model requests are made, on an owned authenticated Unix socket.
Stages run separately so reference and runtime model allocations never overlap.
"""
import argparse
import hashlib
import importlib.metadata
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
sys.path[:0] = [str(ROOT / "tools/model_check"), str(ROOT / "tools")]
import lfm3_check as base
import lfm3_redteam as legacy
from answer_common import GGUF, GGUF_SHA, UnixHTTP, private_socket, sha

UNICODE = [
    "\r\n\r\n  \t\t tabs and CRLF \r\n",
    "Emoji: 😀👩‍👩‍👧‍👦🏳️‍🌈 and flags 🇳🇴🇨🇿",
    "CJK 灯台守の孫娘は霧の中で手紙を読んだ。한국어 문장. 中文句子。",
    "Combining: é ä ñ vs precomposed é ä ñ; zero-width​join‍.",
    "Arabic: المنارة في الضباب. Hebrew: המגדלור בערפל. Hindi: कोहरे में प्रकाशस्तंभ।",
    "Math ∑∫√∞ ≠ ≤ ≥; quotes „“”‘’«» ‹›; dashes ‐‑‒–—―; ellipsis …",
    "   leading and trailing spaces   ",
    "x" * 3000,
]
FLAGS = [
    "-ngl", "99", "-fa", "off", "-ctk", "bf16", "-ctv", "bf16", "--jinja",
    "--reasoning-format", "deepseek", "--no-warmup", "-np", "1", "-c", "8192",
    "-b", "128", "-ub", "64", "-cms", "0", "-ctxcp", "16", "-kvu",
    "--cache-ram", "0", "--no-webui", "--no-slots", "-v",
]
COMMITS = {"baseline": "5851a8e50", "candidate": "03813c56e"}


def save(path, data, replace=False):
    if path.is_symlink():
        raise RuntimeError("Refusing a symlink output")
    with path.open("w" if replace else "x") as file:
        file.write(json.dumps(data, indent=2, allow_nan=False) + "\n")


def call(sock, key, endpoint, body=None, timeout=240):
    connection = UnixHTTP(sock, timeout=timeout)
    try:
        connection.request(
            "GET" if body is None else "POST", endpoint,
            body=None if body is None else json.dumps(body).encode(),
            headers={"Content-Type": "application/json", "Authorization": "Bearer " + key})
        response = connection.getresponse()
        data = response.read()
        if response.status != 200:
            raise RuntimeError(f"{endpoint}: HTTP {response.status}: {data[:500]!r}")
        return json.loads(data)
    finally:
        connection.close()


def metadata():
    return {
        "tree": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "probe_sha256": sha(Path(__file__)),
        "legacy_source_sha256": {p.name: sha(p) for p in
                                (ROOT / "tools/model_check").glob("*.py")},
        "packages": {name: importlib.metadata.version(name) for name in
                     ("torch", "transformers", "numpy", "huggingface_hub", "tokenizers", "safetensors")},
        "python": sys.version,
        "gguf_revision": base.GGUF_REV,
        "reference_revision": base.REV,
        "corpus_sha256": hashlib.sha256(legacy.corpus_text().encode()).hexdigest(),
        "transport_sha256": sha(ROOT / "tools/answer_common.py"),
    }


def runtime(server, directory):
    if sha(GGUF) != GGUF_SHA:
        raise RuntimeError("Pinned GGUF hash mismatch")
    tok = base.tokenizer()
    result = {"metadata": metadata(), "server": str(server), "server_sha256": sha(server),
              "server_version": subprocess.check_output([str(server), "--version"], text=True,
                                                        stderr=subprocess.STDOUT, timeout=30),
              "gguf_sha256": GGUF_SHA, "flags": FLAGS, "short": [], "long": [], "templates": [], "tokenize": []}
    with private_socket() as sock:
        key = secrets.token_hex(24)
        keyfile = sock.parent / "api-key"
        fd = os.open(keyfile, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as file:
            file.write(key + "\n")
        argv = [str(server), "-m", str(GGUF), "--host", str(sock), "--api-key-file", str(keyfile), *FLAGS]
        with (directory / "server.log").open("w") as log:
            process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 180
                while True:
                    if process.poll() is not None:
                        raise RuntimeError(f"Owned server exited during startup: {process.returncode}")
                    try:
                        if call(sock, key, "/health", timeout=1).get("status") == "ok":
                            break
                    except (OSError, RuntimeError):
                        pass
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Owned server readiness timed out")
                    time.sleep(0.25)
                for text, ids in zip(base.prompt_texts(tok), base.prompt_ids(tok)):
                    own = call(sock, key, "/tokenize", {"content": text, "add_special": False,
                                                       "parse_special": True})["tokens"]
                    data = call(sock, key, "/completion", {
                        "prompt": ids, "n_predict": base.N_GEN, "temperature": 0.0, "top_k": 1,
                        "repeat_penalty": 1.0, "n_probs": base.TOP_N, "post_sampling_probs": False,
                        "cache_prompt": False, "return_tokens": True, "samplers": ["top_k"]})
                    result["short"].append({"ids": ids, "own_tokenization": own,
                                             "greedy": data.get("tokens"), "reply": data})
                ids = legacy.long_ids(tok)
                result["long_ids"] = ids
                for position in legacy.POSITIONS:
                    data = call(sock, key, "/completion", {
                        "prompt": ids[:position], "n_predict": 1, "temperature": 0.0, "top_k": 1,
                        "samplers": ["top_k"], "n_probs": base.TOP_N, "post_sampling_probs": False,
                        "cache_prompt": False, "repeat_penalty": 1.0})
                    result["long"].append({"position": position, "reply": data})
                for name, messages, options in legacy.CONVERSATIONS:
                    if name == "history with thinking":
                        messages = [{k: v for k, v in message.items() if k != "reasoning_content"}
                                    for message in messages]
                        name = "history after production reasoning removal"
                    expected = tok.apply_chat_template(messages, tokenize=False, add_generation_prompt=True, **options)
                    body = {"messages": messages, "chat_template_kwargs": options}
                    got = call(sock, key, "/apply-template", body)["prompt"]
                    result["templates"].append({"name": name, "expected": expected, "actual": got,
                                                "identical": got == expected})
                for text in [legacy.corpus_text(), *UNICODE]:
                    expected = tok.encode(text, add_special_tokens=False)
                    got = call(sock, key, "/tokenize", {"content": text, "add_special": False,
                                                       "parse_special": False})["tokens"]
                    result["tokenize"].append({"text": text, "expected": expected, "actual": got,
                                               "identical": got == expected})
            finally:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
    save(directory / "runtime.json", result)


def gap(reference, top):
    if len(reference) < 10 or not all(type(x) in (int, float) and math.isfinite(x) for x in reference):
        raise RuntimeError("Invalid reference distribution")
    candidates = {token["id"]: token["logprob"] for token in top}
    if (len(candidates) != len(top) or not all(
            type(k) is int and 0 <= k < len(reference) and type(v) in (int, float) and math.isfinite(v)
            for k, v in candidates.items())):
        raise RuntimeError("Invalid candidate distribution")
    indices = sorted(range(len(reference)), key=reference.__getitem__, reverse=True)[:10]
    covered = all(index in candidates for index in indices)
    return {"covered": covered,
            "max_gap": max(abs(reference[index] - candidates[index]) for index in indices) if covered else None}


def compare(out, report_path=None):
    short = json.loads((out / "reference/reference.json").read_text())
    long = json.loads((out / "reference/longctx_ref.json").read_text())
    reference_metadata = json.loads((out / "reference/reference-metadata.json").read_text())
    identity = json.loads((out / "reference/identity.json").read_text())
    if (len(short["prompts"]) != 4 or long["positions"] != legacy.POSITIONS or
            len(long["logprobs"]) != len(legacy.POSITIONS) or long["n"] != len(long["ids"]) or
            len(long["ids"]) != max(legacy.POSITIONS)):
        raise RuntimeError("Incomplete reference cases")
    if short["param_dtypes"] != ["torch.float32"] or long["dtype"] != "float32":
        raise RuntimeError("Incorrect reference precision")
    if identity["exit"] != 0 or identity["gguf_sha256"] != GGUF_SHA:
        raise RuntimeError("Artifact identity failed")
    if (reference_metadata["reference_revision"] != base.REV or
            reference_metadata["gguf_revision"] != base.GGUF_REV or
            reference_metadata["packages"]["torch"] != "2.15.0" or
            reference_metadata["packages"]["transformers"] != "5.22.1"):
        raise RuntimeError("Unexpected reference versions")
    if reference_metadata["corpus_sha256"] != hashlib.sha256(legacy.corpus_text().encode()).hexdigest():
        raise RuntimeError("Corpus does not match this checkout")
    if long["metadata"] != reference_metadata or identity["metadata"] != reference_metadata:
        raise RuntimeError("Mixed reference provenance")
    report = {}
    failures = 0
    binary_hashes = set()
    for label in ("baseline", "candidate"):
        data = json.loads((out / label / "runtime.json").read_text())
        if data["metadata"] != reference_metadata or data["long_ids"] != long["ids"]:
            raise RuntimeError("Mixed provenance or corpus observations")
        if data["flags"] != FLAGS or data["gguf_sha256"] != GGUF_SHA:
            raise RuntimeError("Runtime precision/configuration mismatch")
        if COMMITS[label] not in data["server_version"] or len(data["server_sha256"]) != 64:
            raise RuntimeError("Mislabeled runtime binary")
        binary_hashes.add(data["server_sha256"])
        if len(data["short"]) != len(short["prompts"]) or len(data["long"]) != len(long["positions"]):
            raise RuntimeError("Missing runtime observations")
        rows = []
        for expected, actual in zip(short["prompts"], data["short"]):
            if (actual["ids"] != expected["ids"] or not expected["ids"] or
                    len(expected["greedy"]) != base.N_GEN or len(actual["greedy"]) != base.N_GEN):
                raise RuntimeError("Mixed short-prompt observations")
            top = actual["reply"]["completion_probabilities"][0]["top_logprobs"]
            if len(expected["logprobs"]) != 128000 or len(top) != base.TOP_N:
                raise RuntimeError("Incomplete short distributions")
            row = gap(expected["logprobs"], top)
            row["tokenization_match"] = actual["own_tokenization"] == expected["ids"]
            row["pass"] = row["covered"] and row["max_gap"] < 0.05 and row["tokenization_match"]
            rows.append(row)
        long_rows = []
        for position, expected, actual in zip(long["positions"], long["logprobs"], data["long"]):
            if actual["position"] != position:
                raise RuntimeError("Mixed long-context observations")
            top = actual["reply"]["completion_probabilities"][0]["top_logprobs"]
            if len(expected) != 128000 or len(top) != base.TOP_N:
                raise RuntimeError("Incomplete long distributions")
            row = {"position": position, **gap(expected, top)}
            row["pass"] = row["covered"] and row["max_gap"] < 0.12
            long_rows.append(row)
        if (len({x["name"] for x in data["templates"]}) != 5 or
                len({x["text"] for x in data["tokenize"]}) != 9):
            raise RuntimeError("Duplicate or missing template/tokenizer cases")
        names = [("history after production reasoning removal" if name == "history with thinking" else name)
                 for name, _, _ in legacy.CONVERSATIONS]
        if ([x["name"] for x in data["templates"]] != names or
                [x["text"] for x in data["tokenize"]] != [legacy.corpus_text(), *UNICODE]):
            raise RuntimeError("Substituted template/tokenizer cases")
        matches = [x["expected"] == x["actual"] for x in [*data["templates"], *data["tokenize"]]]
        passed = (all(row["pass"] for row in [*rows, *long_rows]) and
                  len(data["templates"]) == 5 and len(data["tokenize"]) == 9 and
                  all(matches))
        report[label] = {"short": rows, "long": long_rows, "pass": passed,
                         "template_matches": sum(x["expected"] == x["actual"] for x in data["templates"]),
                         "tokenizer_matches": sum(x["expected"] == x["actual"] for x in data["tokenize"])}
        failures += not passed
        print(label, json.dumps(report[label]), flush=True)
    if len(binary_hashes) != 2:
        raise RuntimeError("Baseline/candidate must be distinct binaries")
    save(report_path or out / "comparison.json", report)
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("stage", choices=("identity", "reference-short", "reference-long", "runtime", "compare"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--server", type=Path)
    parser.add_argument("--label", choices=("baseline", "candidate"))
    parser.add_argument("--report", type=Path, help="New report path; never overwrite existing evidence")
    args = parser.parse_args()
    os.chdir(ROOT)
    if os.environ.get("HF_HUB_OFFLINE") != "1":
        raise RuntimeError("Use cached pinned artifacts only: HF_HUB_OFFLINE=1 is required")
    if args.stage == "compare":
        return compare(args.out, args.report)
    active = subprocess.run(["pgrep", "-x", "llama-server"], stdout=subprocess.PIPE, text=True)
    if active.returncode not in (0, 1) or active.stdout.strip():
        raise RuntimeError("An existing model server is present; leave it untouched and run when idle")
    if args.stage == "runtime" and (not args.server or not args.label):
        raise RuntimeError("Runtime stage requires explicit server and label")
    directory = args.out / (args.label if args.stage == "runtime" else "reference")
    directory.mkdir(parents=True, exist_ok=True)
    base.OUT_DIR = str(directory)
    legacy.OUT = str(directory)
    expected = {"identity": "identity.json", "reference-short": "reference.json",
                "reference-long": "longctx_ref.json", "runtime": "runtime.json"}[args.stage]
    if (directory / expected).exists():
        raise RuntimeError("Refusing to overwrite a previous observation")
    if args.stage == "identity":
        import gguf_identity
        original_argv = sys.argv
        try:
            sys.argv = [gguf_identity.__file__, base.GGUF_FILE]
            result = gguf_identity.main()
        finally:
            sys.argv = original_argv
        save(directory / "identity.json", {"metadata": metadata(), "exit": result, "gguf_sha256": sha(GGUF)})
        return result
    if args.stage == "runtime":
        if not args.server or not args.label:
            raise RuntimeError("Runtime stage requires explicit server and label")
        runtime(args.server.resolve(), directory)
    elif args.stage == "reference-short":
        base.stage_reference()
        save(directory / "reference-metadata.json", metadata())
    else:
        legacy.longctx_ref()
        path = directory / "longctx_ref.json"
        data = json.loads(path.read_text())
        data.update({"metadata": metadata(), "ids": legacy.long_ids(base.tokenizer())})
        save(path, data, replace=True)
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    sys.exit(main())
