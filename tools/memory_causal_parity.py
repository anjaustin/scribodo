#!/usr/bin/env python3
"""Exact-input local reference/llama diagnostics, never a product quality gate.

Run prepare, llama, reference, compare sequentially with the diagnostic venv.
Only cached pinned model artifacts are used. No author files or remote inference.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
REV = "0dc4268b561499c648c7ba7c9466bf3d8d01517a"
SNAPSHOT = Path.home() / ".cache/huggingface/hub/models--LiquidAI--LFM3-1.7B/snapshots" / REV
GGUF = Path.home() / ".cache/huggingface/hub/models--LiquidAI--LFM3-1.7B-GGUF/snapshots/cd0c74c575b5c416f3011b5b4c298077f26ca8c2/LFM3-1.7B-BF16.gguf"
GGUF_SHA = "39158a79f56edb85e0c4252ce6447b8d7883ef82ffedd02f0cda7c84da5e9f68"
CODE = ("chat_template.jinja", "config.json", "generation_config.json", "configuration_lfm3.py",
        "modeling_lfm3.py", "lfm3_conv.py", "tokenizer.json", "tokenizer_config.json")
SELECTED = (1, 4, 26)  # Passing fact, repeated-history failure, author-ambiguity failure.


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1048576), b""):
            h.update(chunk)
    return h.hexdigest()


def write(path, data):
    # Refuse to overwrite earlier evidence on a repeated stage invocation.
    with path.open("x") as file:
        json.dump(data, file, ensure_ascii=False, indent=2)


def tokenizer():
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    from transformers import AutoTokenizer
    return AutoTokenizer.from_pretrained(str(SNAPSHOT), local_files_only=True, trust_remote_code=True)


def prepare(source):
    rows = [r for line in (source / "events.jsonl").read_text().splitlines()
            if (r := json.loads(line)).get("event") == "answer"]
    if len(rows) != 42:
        raise RuntimeError("Expected the complete frozen native DEV corpus")
    run = Path(tempfile.mkdtemp(prefix="mc-", dir=ROOT / "build"))
    run.chmod(0o700)
    tok = tokenizer()
    cases = []
    for index in SELECTED:
        row = rows[index]
        request = json.loads(row["request"])
        if request["max_tokens"] != 1024 or request["temperature"] != 0 or request["repeat_penalty"] != 1.05:
            raise RuntimeError("Unexpected native sampling; do not silently rematch")
        rendered = tok.apply_chat_template(request["messages"], tokenize=False, add_generation_prompt=True,
                                          enable_thinking=False)
        ids = tok.encode(rendered, add_special_tokens=False)
        cases.append({"index": index, "task": row["task"], "layout": row["layout"],
                      "presentation": row["presentation"], "request": request,
                      "native_answer": row["answer"], "native_truncated": row["truncated"],
                      "reference_rendered": rendered, "ids": ids, "max_new_tokens": 1024})
    write(run / "cases.json", cases)
    shutil.copyfile(__file__, run / "parity.py")
    shutil.copyfile(ROOT / "tools/memory_bedrock_probe.py", run / "memory_bedrock_probe.py")
    write(run / "manifest.json", {
        "kind": "causal_diagnostic_not_quality_gate", "revision": REV,
        "source_events_sha256": sha(source / "events.jsonl"), "source_run": str(source),
        "selected_native_indices": list(SELECTED), "cases_sha256": sha(run / "cases.json"),
        "script_sha256": sha(Path(__file__)), "official_files_sha256": {name: sha(SNAPSHOT / name) for name in CODE},
        "helper_sha256": sha(ROOT / "tools/memory_bedrock_probe.py"),
        "sampler": "greedy; repetition penalty 1.05 on last 64 tokens in both runtimes; no prompt reuse",
        "intent": "Exact native messages, official disabled-thinking template, matched local decoding; no prompt tuning",
        "limits": "Three DEV cases only; F32 reference vs BF16-weight/F32-activation llama; near-tie divergence is not proof of a runtime defect"
    })
    print(run, flush=True)


def validate(run):
    manifest = json.loads((run / "manifest.json").read_text())
    if sha(Path(__file__)) != manifest["script_sha256"]:
        raise RuntimeError("Diagnostic script changed; prepare a new run")
    if sha(Path(__file__).with_name("memory_bedrock_probe.py")) != manifest["helper_sha256"]:
        raise RuntimeError("Diagnostic helper changed; prepare a new run")
    if sha(run / "cases.json") != manifest["cases_sha256"]:
        raise RuntimeError("Frozen cases changed")
    for name, expected in manifest["official_files_sha256"].items():
        if sha(SNAPSHOT / name) != expected:
            raise RuntimeError("Official artifact changed: " + name)
    return json.loads((run / "cases.json").read_text())


def common(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return min(len(a), len(b))


def llama(run):
    from memory_bedrock_probe import request
    cases = validate(run)
    if sha(GGUF) != GGUF_SHA:
        raise RuntimeError("GGUF identity mismatch")
    binary = ROOT / "build/llama/bin/llama-server"
    sock = run / "llama.sock"
    if len(os.fsencode(sock)) >= 104:
        raise RuntimeError("Socket path too long")
    keyfile = run / "api.key"
    key = secrets.token_hex(32)
    keyfile.write_text(key)
    keyfile.chmod(0o600)
    command = [str(binary), "-m", str(GGUF), "--host", str(sock), "--api-key-file", str(keyfile),
               "-ngl", "99", "-fa", "off", "-ctk", "bf16", "-ctv", "bf16", "--jinja",
               "--reasoning-format", "deepseek", "-np", "2", "-c", "16384", "-b", "256", "-ub", "256",
               "--cache-ram", "0", "--no-webui", "--no-slots"]
    write(run / "llama-runtime.json", {"command": command, "binary_sha256": sha(binary), "gguf_sha256": GGUF_SHA})
    with (run / "llama-server.log").open("xb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 120
            while True:
                if process.poll() is not None:
                    raise RuntimeError("Owned llama server exited")
                try:
                    request(sock, key, "/health")
                    break
                except (OSError, RuntimeError):
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Owned server startup timeout")
                    time.sleep(0.1)
            write(run / "llama-props.json", request(sock, key, "/props"))
            for case in cases:
                index = case["index"]
                rendered = request(sock, key, "/apply-template", case["request"])["prompt"]
                own_ids = request(sock, key, "/tokenize", {"content": rendered, "add_special": False, "parse_special": True})["tokens"]
                identical = rendered == case["reference_rendered"] and own_ids == case["ids"]
                write(run / f"template-{index}.json", {"rendered": rendered, "ids": own_ids,
                                                       "bytes_match": rendered == case["reference_rendered"],
                                                       "tokens_match": own_ids == case["ids"]})
                if not identical:
                    raise RuntimeError(f"Template/token mismatch at case {index}; do not call inference matched")
                start = time.monotonic()
                result = request(sock, key, "/completion", {
                    "prompt": case["ids"], "n_predict": case["max_new_tokens"], "temperature": 0,
                    "top_k": 1, "samplers": ["penalties", "top_k"], "repeat_penalty": 1.05, "repeat_last_n": 64,
                    "presence_penalty": 0, "frequency_penalty": 0, "cache_prompt": False,
                    "id_slot": 0, "return_tokens": True, "n_probs": 64, "post_sampling_probs": False})
                write(run / f"llama-{index}.json", {"seconds": time.monotonic() - start, "response": result})
                print(f"llama case {index}: {result.get('tokens_predicted')} tokens, {result.get('stop_type')}", flush=True)
        finally:
            process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            keyfile.unlink(missing_ok=True)


def reference(run):
    cases = validate(run)
    tok = tokenizer()
    import torch
    import transformers
    from transformers import AutoModelForCausalLM, GenerationConfig, LogitsProcessor, LogitsProcessorList

    class WindowPenalty(LogitsProcessor):
        def __call__(self, input_ids, scores):
            recent = input_ids[:, -64:]
            gathered = torch.gather(scores, 1, recent)
            adjusted = torch.where(gathered < 0, gathered * 1.05, gathered / 1.05)
            return scores.scatter(1, recent, adjusted)

    torch.set_num_threads(4)
    weights_sha256 = sha(SNAPSHOT / "model.safetensors")
    model = AutoModelForCausalLM.from_pretrained(str(SNAPSHOT), local_files_only=True, trust_remote_code=True,
                                               dtype=torch.float32, attn_implementation="sdpa")
    model.eval()
    write(run / "reference-runtime.json", {"torch": torch.__version__, "transformers": transformers.__version__,
                                          "device": "CPU", "dtype": "float32 widened from cached official BF16",
                                          "weights_sha256": weights_sha256,
                                          "threads": 4, "use_cache": True, "attention": "sdpa"})
    with torch.inference_mode():
        for case in cases:
            index = case["index"]
            inputs = torch.tensor([case["ids"]])
            config = GenerationConfig(max_new_tokens=case["max_new_tokens"], do_sample=False,
                                      repetition_penalty=1.0, use_cache=True, bos_token_id=model.config.bos_token_id,
                                      eos_token_id=model.config.eos_token_id, pad_token_id=model.config.pad_token_id)
            start = time.monotonic()
            generated = model.generate(inputs, generation_config=config, logits_processor=LogitsProcessorList([WindowPenalty()]),
                                       logits_to_keep=1)[0, len(case["ids"]):].tolist()
            record = {"seconds": time.monotonic() - start, "tokens": generated,
                      "text": tok.decode(generated, skip_special_tokens=False),
                      "truncated": len(generated) == case["max_new_tokens"] and generated[-1] != model.config.eos_token_id}
            # Compare distributions on a shared prefix, not different histories
            # after their first greedy divergence. Raw probabilities precede penalties.
            candidate = json.loads((run / f"llama-{index}.json").read_text())["response"]
            prefix = common(generated, candidate.get("tokens", []))
            positions = sorted({0, prefix}) if prefix < min(len(generated), len(candidate.get("tokens", []))) else [0]
            record["shared_greedy_prefix"] = prefix
            record["distributions"] = []
            for position in positions:
                sequence = torch.tensor([case["ids"] + generated[:position]])
                logits = model(sequence, use_cache=False, logits_to_keep=1).logits[0, -1].float()
                logprobs = torch.log_softmax(logits, -1)
                values, ids = torch.topk(logprobs, 64)
                row = {"position": position, "top": [{"id": i, "logprob": p} for i, p in zip(ids.tolist(), values.tolist())]}
                probs = candidate.get("completion_probabilities", [])
                if position < len(probs):
                    cmap = {v["id"]: v["logprob"] for v in probs[position]["top_logprobs"]}
                    rtop = row["top"][:10]
                    row["reference_top10_covered"] = all(v["id"] in cmap for v in rtop)
                    row["max_top10_logprob_gap"] = max((abs(v["logprob"] - cmap[v["id"]]) for v in rtop if v["id"] in cmap), default=None)
                    row["reference_top1_margin"] = row["top"][0]["logprob"] - row["top"][1]["logprob"]
                record["distributions"].append(row)
            write(run / f"reference-{index}.json", record)
            print(f"reference case {index}: {len(generated)} tokens; shared prefix {prefix}", flush=True)


def compare(run):
    rows = []
    for case in validate(run):
        index = case["index"]
        candidate = json.loads((run / f"llama-{index}.json").read_text())["response"]
        ref = json.loads((run / f"reference-{index}.json").read_text())
        rows.append({"index": index, "task": case["task"], "layout": case["layout"],
                     "template": json.loads((run / f"template-{index}.json").read_text()),
                     "native_answer": case["native_answer"], "llama_answer": candidate.get("content"),
                     "reference_answer": ref["text"], "reference_truncated": ref["truncated"],
                     "shared_greedy_prefix": ref["shared_greedy_prefix"], "distributions": ref["distributions"]})
    write(run / "comparison.json", {"kind": "manual_semantic_and_runtime_review_required", "quality_pass": False, "cases": rows})
    print("Comparison saved; no automatic root-cause or quality pass", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("prepare", "llama", "reference", "compare"))
    parser.add_argument("path", type=Path, help="Native source run for prepare; causal run for other stages")
    args = parser.parse_args()
    path = args.path.resolve()
    if args.stage == "prepare":
        prepare(path)
    else:
        {"llama": llama, "reference": reference, "compare": compare}[args.stage](path)


if __name__ == "__main__":
    main()
