#!/usr/bin/env python3
"""Preserve C03 model observations and their source/configuration snapshots, without weights."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tarfile

ROOT = Path(__file__).resolve().parents[3]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    h = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(4 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def regular_tree(directory):
    if directory.is_symlink() or not directory.is_dir():
        raise RuntimeError("A real, explicitly admitted run directory is required")
    for path in sorted(directory.rglob("*")):
        if path.is_symlink():
            raise RuntimeError("Symlinks are not admitted to evidence trees")
        if path.is_file():
            yield path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--candidate-build", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--evidence", type=Path,
                        default=Path.home() / "Library/Application Support/Scribodo/evidence")
    args = parser.parse_args()
    if not re.fullmatch(r"scribodo-evidence-[a-z0-9-]+", args.name):
        raise RuntimeError("Archive name must be a basename")
    if not args.evidence.is_dir():
        raise RuntimeError("Existing evidence directory required")
    archive = args.evidence / (args.name + ".tar.gz")
    manifest = args.evidence / ("manifest-" + args.name + ".json")
    sidecar = args.evidence / (args.name + ".sha256")
    if any(path.exists() or path.is_symlink() for path in (archive, manifest, sidecar)):
        raise RuntimeError("Refusing to overwrite archive, manifest or checksum")
    entries = {}
    for path in regular_tree(args.run):
        if path.suffix not in (".json", ".log", ".tsv", ".py", ".txt", ".sh", ".diff"):
            raise RuntimeError("Unexpected run artifact; review before archival")
        entries["c03/" + args.run.name + "/" + str(path.relative_to(args.run))] = path
    for path in regular_tree(Path(__file__).parent):
        # Only known public-source evidence and explicitly safe file types; no credentials or binaries.
        if ("__pycache__" not in path.parts and path.name != "grammar_rejection.cpp" and
                path.suffix in (".py", ".sh", ".md", ".json", ".log", ".txt", ".diff", ".patch", ".tsv")):
            entries["source/" + str(path.relative_to(ROOT))] = path
    for path in [*sorted((ROOT / "tools/model_check").glob("*.py")), ROOT / "tools/answer_common.py",
                  ROOT / "tools/watchdog.sh", ROOT / "tools/envs/venv-causal.lock",
                  ROOT / "tools/envs/venv-coref.lock", ROOT / "Makefile",
                  ROOT / "tests/test_c03_acceptance.py", ROOT / "tools/answer_unit_extraction.py",
                  ROOT / "tools/answer_rerank_arm.py"]:
        entries["source/" + str(path.relative_to(ROOT))] = path
    entries["c03/candidate-build/CMakeCache.txt"] = args.candidate_build / "CMakeCache.txt"
    for path in sorted(args.candidate_build.rglob("link.txt")):
        entries["c03/candidate-build/" + str(path.relative_to(args.candidate_build))] = path
    snapshot = (Path.home() / ".cache/huggingface/hub/models--LiquidAI--LFM3-1.7B/snapshots/"
                "0dc4268b561499c648c7ba7c9466bf3d8d01517a")
    for path in sorted(snapshot.iterdir()):
        if path.suffix == ".py" or path.name in ("config.json", "tokenizer_config.json", "generation_config.json"):
            entries["c03/reference-source/" + path.name] = path
    expected = {name: {"sha256": file_digest(path), "bytes": path.stat().st_size}
                for name, path in entries.items()}
    with tarfile.open(archive, "x:gz") as tar:
        for name, path in entries.items():
            tar.add(path.resolve(), arcname=name, recursive=False)
    with tarfile.open(archive, "r:gz") as tar:
        actual = {member.name: {"sha256": digest(tar.extractfile(member).read()), "bytes": member.size}
                  for member in tar.getmembers()}
    if actual != expected:
        raise RuntimeError("Archive read-back mismatch")
    sha = file_digest(archive)
    with manifest.open("x") as file:
        json.dump({"archive_sha256": sha, "files": expected}, file, indent=2)
        file.write("\n")
    with sidecar.open("x") as file:
        file.write(sha + "  " + archive.name + "\n")
    print(json.dumps({"archive": str(archive), "sha256": sha, "bytes": archive.stat().st_size,
                      "files": len(expected), "manifest": str(manifest),
                      "verification": "all member bytes rehashed without extraction"}, indent=2))


if __name__ == "__main__":
    main()
