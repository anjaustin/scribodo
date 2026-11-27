#!/usr/bin/env python3
"""Offline postflight of the C03 observations; never loads a model or starts a server."""
import hashlib
import json
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
RUN = None
REFERENCE_REV = "0dc4268b561499c648c7ba7c9466bf3d8d01517a"
GGUF_REV = "cd0c74c575b5c416f3011b5b4c298077f26ca8c2"
GGUF_SHA = "39158a79f56edb85e0c4252ce6447b8d7883ef82ffedd02f0cda7c84da5e9f68"
POSITIONS = [200, 255, 256, 257, 300, 512, 1024, 2048, 3072, 4096]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(4 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load(name):
    return json.loads((RUN / name).read_text())


def distribution(reference, reply):
    require(len(reference) == 128000 and all(math.isfinite(x) for x in reference),
            "Reference must contain the complete finite vocabulary")
    # MODEL-001 gates log-probability gaps, not re-normalization in Python F64.
    # Report the F32 reference's normalization residual without changing its values.
    top = reply["completion_probabilities"][0]["top_logprobs"]
    got = {x["id"]: x["logprob"] for x in top}
    require(len(top) == len(got) == 20 and
            all(type(k) is int and 0 <= k < len(reference) and math.isfinite(v) for k, v in got.items()),
            "Invalid runtime top-20")
    best = sorted(range(len(reference)), key=reference.__getitem__, reverse=True)[:10]
    require(all(i in got for i in best), "Missing reference top-10")
    return max(abs(reference[i] - got[i]) for i in best)


def main():
    global RUN
    if len(sys.argv) != 2:
        raise RuntimeError("Usage: verify_parity.py RUN")
    RUN = Path(sys.argv[1])
    short = load("reference/reference.json")
    long = load("reference/longctx_ref.json")
    metadata = load("reference/reference-metadata.json")
    identity = load("reference/identity.json")
    require(identity["exit"] == 0 and identity["gguf_sha256"] == GGUF_SHA, "Identity failure")
    require(short["param_dtypes"] == ["torch.float32"] and long["dtype"] == "float32",
            "Reference precision changed")
    require(len(short["prompts"]) == 4 and long["positions"] == POSITIONS and
            len(long["logprobs"]) == 10, "Missing reference observations")
    require(long["n"] == len(long["ids"]), "Long reference length mismatch")
    all_metadata = [metadata, long["metadata"], identity["metadata"]]
    report = {}
    saved = load("comparison.json")
    for label in ("baseline", "candidate"):
        data = load(f"{label}/runtime.json")
        all_metadata.append(data["metadata"])
        require(data["gguf_sha256"] == GGUF_SHA and data["server_sha256"] == digest(Path(data["server"])),
                "Runtime binary/artifact identity changed")
        require(len(data["short"]) == 4 and len(data["long"]) == 10 and
                data["long_ids"] == long["ids"], "Missing/mixed runtime observations")
        short_gaps, long_gaps, greedy = [], [], []
        for ref, actual in zip(short["prompts"], data["short"]):
            require(ref["ids"] == actual["ids"] == actual["own_tokenization"], "Prompt token mismatch")
            value = distribution(ref["logprobs"], actual["reply"])
            require(value < 0.05, "Short distribution gate failed")
            short_gaps.append(value)
            require(len(ref["greedy"]) == len(actual["greedy"]) == 40, "Missing greedy continuation")
            greedy.append(sum(a == b for a, b in zip(ref["greedy"], actual["greedy"])))
        for position, ref, actual in zip(POSITIONS, long["logprobs"], data["long"]):
            require(actual["position"] == position, "Position mismatch")
            value = distribution(ref, actual["reply"])
            require(value < 0.12, "Long distribution gate failed")
            long_gaps.append(value)
        require(len(data["templates"]) == 5 and
                all(x["identical"] and x["expected"] == x["actual"] for x in data["templates"]),
                "Template gate failed")
        require(len(data["tokenize"]) == 9 and
                all(x["identical"] and x["expected"] == x["actual"] for x in data["tokenize"]),
                "Tokenization gate failed")
        require(short_gaps == [x["max_gap"] for x in saved[label]["short"]] and
                long_gaps == [x["max_gap"] for x in saved[label]["long"]] and saved[label]["pass"],
                "Stored comparison differs from independent recomputation")
        report[label] = {"worst_short_gap": max(short_gaps), "worst_long_gap": max(long_gaps),
                         "greedy_same_positions_out_of_40": greedy, "templates": 5, "tokenizer_samples": 9}
    for meta in all_metadata:
        require(meta == metadata, "Provenance differs across stages")
        require(meta["reference_revision"] == REFERENCE_REV and meta["gguf_revision"] == GGUF_REV,
                "Wrong artifact revisions")
        require(meta["packages"]["torch"] == "2.15.0" and meta["packages"]["transformers"] == "5.22.1",
                "Wrong reference packages")
        require(meta["probe_sha256"] == digest(Path(__file__).with_name("compatibility_probe.py")),
                "Probe changed after measurement")
        for name, expected in meta["legacy_source_sha256"].items():
            require(expected == digest(ROOT / "tools/model_check" / name), "Reference helper changed")
    print(json.dumps({"pass": True, "reference_param_dtypes": short["param_dtypes"],
                      "reference_long_dtype": long["dtype"], "results": report,
                      "reference_normalization_residuals_diagnostic_only": {
                          "short": [sum(math.exp(x) for x in p["logprobs"]) - 1 for p in short["prompts"]],
                          "long": [sum(math.exp(x) for x in row) - 1 for row in long["logprobs"]]},
                      "raw_files": {str(p.relative_to(RUN)): {"sha256": digest(p), "bytes": p.stat().st_size}
                                    for p in sorted(RUN.rglob("*")) if p.is_file()}}, indent=2))


if __name__ == "__main__":
    main()
