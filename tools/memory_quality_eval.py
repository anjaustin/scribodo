#!/usr/bin/env python3
"""Preregistered matched-arm local-model experiment; stdlib only.

Default: 12 DEV cases / >=10K words (not heldout evidence).
Long: --heldout --words 100000 (or opt-in 300000). All outputs are retained.
--prepare-only freezes inputs without starting services or running inference.
--self-test tests the scorer/corpus invariants only, never model quality.
No fixture, prompt, or scorer adaptation after observing heldout failures.
"""
import argparse
import hashlib
import inspect
import json
import os
from pathlib import Path
import random
import re
import secrets
import shutil
import subprocess
import sys
import time
import unicodedata

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/fixtures/memory_quality"
ARMS = ("recent_window", "retrieval_only", "t1_retrieval", "oracle_source")


def normalized(value):
    return " ".join(unicodedata.normalize("NFKC", value).casefold().split())


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def implementation_sources():
    paths = [ROOT / "tools/memory_quality_eval.c"]
    for folder in ("src/app", "src/core", "src/llm"):
        paths.extend(p for p in (ROOT / folder).glob("*") if p.suffix in (".c", ".h"))
    return sorted(paths)


def snapshot_build(binary):
    """Content-addressed owned executable, independent of later parent links."""
    for _ in range(3):
        before = binary.stat()
        data = binary.read_bytes()
        after = binary.stat()
        if (before.st_ino, before.st_size, before.st_mtime_ns) == (after.st_ino, after.st_size, after.st_mtime_ns):
            break
    else:
        raise RuntimeError("native binary changed during build snapshot; rebuild before launch")
    digest = hashlib.sha256(data).hexdigest()
    artifact = ROOT / "build/memory-quality/artifacts" / digest
    artifact.mkdir(parents=True, exist_ok=True)
    executable = artifact / "memory_quality_eval"
    if not executable.exists():
        executable.write_bytes(data)
        executable.chmod(0o500)
    if sha(executable) != digest:
        raise RuntimeError("owned build artifact digest mismatch")
    manifest_path = artifact / "build-manifest.json"
    if not manifest_path.exists():
        hashes = {}
        for source in implementation_sources():
            relative = source.relative_to(ROOT)
            content = source.read_bytes()
            target = artifact / "sources" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
            target.chmod(0o400)
            hashes[str(relative)] = hashlib.sha256(content).hexdigest()
        save(manifest_path, {"binary_sha256": digest, "implementation_source_sha256": hashes,
                            "source_snapshot_timing": "immediately after link; compiler/object concurrency not attested",
                            "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())})
        manifest_path.chmod(0o400)
    save(ROOT / "build/memory-quality/build-manifest.json",
         {"artifact": str(artifact), "build_manifest_sha256": sha(manifest_path), "binary_sha256": digest})
    return executable, manifest_path


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def strict_json(text):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("duplicate JSON key")
            result[key] = value
        return result
    return json.loads(text, object_pairs_hook=pairs,
                      parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))


def corpus(fixture, seed, words):
    """Deterministic fictional prose, original facts scattered before recent64.

    The archive, not any answer key, goes into native ingestion. No source is
    labelled privileged/canonical. Facts have short original-only passages so
    the oracle never needs a larger budget than other arms.
    """
    rng = random.Random(seed)
    places = ["quay", "harbour", "garden", "courtyard", "market", "shore", "hillside"]
    textures = ["quiet", "misty", "weathered", "gentle", "distant", "pale", "wooden"]
    objects = ["reeds", "clouds", "ropes", "shutters", "leaves", "cobbles", "sails"]
    archive = []
    # >=100 words per paragraph; each is comfortably below the 2000-byte cap.
    paragraph_count = (words + 99) // 100
    for number in range(paragraph_count):
        sentences = []
        while len(" ".join(sentences).split()) < 100:
            sentences.append(
                f"Beyond the {rng.choice(textures)} {rng.choice(places)}, the travelers watched "
                f"the {rng.choice(objects)} and listened to the wind moving through the evening air."
            )
        archive.append({"id": f"prose:{number}", "text": " ".join(sentences)})
    # Fixed ordering preserves corrections; insertion depends only on seed/size,
    # never on question text or expected-answer contents.
    n = len(fixture["sources"])
    positions = [int((i + 1) * paragraph_count * .82 / (n + 1)) for i in range(n)]
    for index, (position, source) in enumerate(zip(positions, fixture["sources"])):
        archive.insert(position + index, dict(source))
    for i in range(64):
        archive.append({"id": f"tail:{i}", "text":
                        "Recent manuscript turn: the breeze crossed the quay; ordinary weather observations only."})
    for sequence, row in enumerate(archive):
        row["sequence"] = sequence
    mapping = {row["id"]: row["sequence"] for row in archive}
    tasks = []
    for task in fixture["tasks"]:
        tasks.append({"id": task["id"], "question": task["question"],
                      "oracle_sequences": [mapping[source] for source in task["sources"]]})
    recent = [{"id": f"recent:{row['sequence']}", "sequence": row["sequence"], "text": row["text"]}
              for row in archive[-64:]]
    actual_words = sum(len(row["text"].split()) for row in archive)
    assert actual_words >= words
    assert all(len(row["text"].encode()) <= 2000 for row in archive)
    boundary = len(archive) - 64
    assert all(sequence < boundary for task in tasks for sequence in task["oracle_sequences"])
    recent_text = normalized(" ".join(row["text"] for row in recent))
    for task in fixture["tasks"]:
        for alias in task["answers"]:
            # Token boundaries avoid mistaking 14 for a substring of 2014.
            pattern = r"(?<!\w)" + re.escape(normalized(alias)) + r"(?!\w)"
            assert not re.search(pattern, normalized(task["question"])), (task["id"], "question leak")
            assert not re.search(pattern, recent_text), (task["id"], "recent leak")
    return archive, recent, tasks, actual_words


def score_answer(task, event, archive):
    result = {"task": task["id"], "arm": event["arm"], "correct": False,
              "citation_valid": 0, "citation_total": 0, "evidence_shape_ok": False,
              "privacy_violations": 0, "source_recall": None,
              "pointer_only_provenance_coverage": None, "raw_or_pointer_coverage": None,
              "raw_citations": 0, "pointer_only_citations": 0}
    refs = event.get("admitted", {}).get("originals", [])
    admitted = {}
    selected = set()
    reference = event.get("production_reference", "")
    for ref in refs:
        sequence = ref.get("sequence")
        if (not isinstance(sequence, int) or isinstance(sequence, bool) or
                not 0 <= sequence < len(archive) or not isinstance(ref.get("text"), str) or
                not ref["text"] or ref["text"] not in archive[sequence]["text"] or
                ref["text"] not in reference or f"unit={ref.get('id')}" not in reference):
            result["privacy_violations"] += 1
            continue
        if ref.get("id") in admitted and admitted[ref["id"]] != ref["text"]:
            result["privacy_violations"] += 1
        admitted[ref["id"]] = ref["text"]
        selected.add(sequence)
    pointer_admitted = {}
    pointer_sequences = set()
    for pointer in event.get("admitted", {}).get("brief_input_pointers", []):
        try:
            sequence = pointer["sequence"]
            start, length = pointer["byte_start"], pointer["text_bytes"]
            if (any(not isinstance(x, int) or isinstance(x, bool) for x in (sequence, start, length)) or
                    not 0 <= sequence < len(archive) or start < 0 or length <= 0):
                raise ValueError("invalid pointer range")
            source = archive[sequence]["text"].encode("utf-8")
            text_bytes = source[start:start + length]
            if (len(text_bytes) != length or hashlib.sha256(text_bytes).hexdigest() != pointer["text_hash"] or
                    f"unit={pointer['id']}" not in reference or f"brief={pointer['brief_id']}" not in reference):
                raise ValueError("invalid production pointer")
            pointer_admitted[pointer["id"]] = text_bytes.decode("utf-8")
            pointer_sequences.add(sequence)
        except (ValueError, KeyError, TypeError, UnicodeError):
            result["privacy_violations"] += 1
    required = set(task["required_sequences"])
    if required:
        result["source_recall"] = len(required & selected) / len(required)
        result["pointer_only_provenance_coverage"] = len(required & (pointer_sequences - selected)) / len(required)
        result["raw_or_pointer_coverage"] = len(required & (pointer_sequences | selected)) / len(required)
    canary = "PRIVATE-OTHER-CONVERSATION-9b174c6f"
    if canary in event.get("final_request", "") or canary in event.get("response", ""):
        result["privacy_violations"] += 1
    try:
        answer = strict_json(event.get("response", ""))
        if not isinstance(answer, dict) or set(answer) != {"status", "answer", "evidence"}:
            raise ValueError("answer schema")
        if not isinstance(answer["answer"], str) or not isinstance(answer["evidence"], list):
            raise ValueError("answer types")
        evidence = answer["evidence"]
        result["citation_total"] = len(evidence)
        all_originals = dict(pointer_admitted, **admitted)
        for citation in evidence:
            if (isinstance(citation, dict) and set(citation) == {"id", "quote"} and
                    isinstance(citation["id"], str) and isinstance(citation["quote"], str) and
                    citation["quote"] and citation["id"] in all_originals and
                    citation["quote"] in all_originals[citation["id"]]):
                result["citation_valid"] += 1
                result["raw_citations" if citation["id"] in admitted else "pointer_only_citations"] += 1
        result["evidence_shape_ok"] = (result["citation_valid"] == len(evidence) and
                                        (bool(evidence) if answer["status"] == "answer" else not evidence))
        if task.get("abstain"):
            result["correct"] = (answer["status"] == "abstain" and answer["answer"] == "" and evidence == [])
        else:
            result["correct"] = (answer["status"] == "answer" and bool(evidence) and
                                  normalized(answer["answer"]) in {normalized(x) for x in task["answers"]})
        # Answer correctness and source citation precision are deliberately separate.
        result["parsed_answer"] = answer
    except (ValueError, TypeError, KeyError):
        result["parse_error"] = True
    usage = event.get("generator_usage")
    result["matched_runtime_ok"] = (
        event.get("transport_ok") is True and event.get("source_tokens", 999999) <= 4096 and
        0 < event.get("final_prompt_tokens", 999999) <= 8064 and isinstance(usage, dict) and
        isinstance(usage.get("completion_tokens"), int) and 0 <= usage["completion_tokens"] <= 128 and
        usage.get("prompt_tokens") == event.get("final_prompt_tokens"))
    result["correct"] = result["correct"] and result["matched_runtime_ok"]
    return result


def summarize(manifest, fixture, native_input, events, driver_rc):
    mapping = {row["id"]: row["sequence"] for row in native_input["archive"]}
    taskmap = {}
    for task in fixture["tasks"]:
        taskmap[task["id"]] = dict(task, required_sequences=[mapping[s] for s in task["sources"]])
    answers = [event for event in events if event.get("event") == "answer"]
    scored = [score_answer(taskmap[event["task"]], event, native_input["archive"])
              for event in answers if event.get("task") in taskmap and event.get("arm") in ARMS]
    arms = {}
    for arm in ARMS:
        rows = [row for row in scored if row["arm"] == arm]
        citations = sum(row["citation_total"] for row in rows)
        valid = sum(row["citation_valid"] for row in rows)
        recall = [row["source_recall"] for row in rows if row["source_recall"] is not None]
        pointer_coverage = [row["pointer_only_provenance_coverage"] for row in rows if row["pointer_only_provenance_coverage"] is not None]
        combined_coverage = [row["raw_or_pointer_coverage"] for row in rows if row["raw_or_pointer_coverage"] is not None]
        arms[arm] = {"n": len(rows), "correct": sum(row["correct"] for row in rows),
                     "correctness_and_abstention": sum(row["correct"] for row in rows) / len(taskmap),
                     "source_selection_macro_recall": sum(recall) / len(recall) if recall else None,
                     "source_selection_metric": "actual raw-original excerpts only; brief pointers excluded",
                     "pointer_only_provenance_macro_coverage": sum(pointer_coverage) / len(pointer_coverage) if pointer_coverage else None,
                     "raw_or_pointer_macro_coverage": sum(combined_coverage) / len(combined_coverage) if combined_coverage else None,
                     "raw_citations": sum(row["raw_citations"] for row in rows),
                     "pointer_only_citations": sum(row["pointer_only_citations"] for row in rows),
                     "source_evidence_exact_quote_id_precision": valid / citations if citations else None,
                     "cited_quotes": citations,
                     "evidence_shape_failures": sum(not row["evidence_shape_ok"] for row in rows),
                     "privacy_violations": sum(row["privacy_violations"] for row in rows)}
    complete = [e for e in events if e.get("event") == "complete"]
    background = [e for e in events if e.get("event") == "background"]
    costs = {}
    for arm in ARMS:
        receipts = [e for e in answers if e.get("arm") == arm]
        known = [e["generator_usage"] for e in receipts if isinstance(e.get("generator_usage"), dict)]
        costs[arm] = {
            "foreground_requests": len(receipts), "foreground_usage_known_requests": len(known),
            "foreground_prompt_tokens_known": sum(u.get("prompt_tokens", 0) for u in known),
            "foreground_completion_tokens_known": sum(u.get("completion_tokens", 0) for u in known),
            "generation_seconds": sum(e.get("generation_seconds", 0) for e in receipts),
            "tokenization_seconds": sum(e.get("tokenization_seconds", 0) for e in receipts),
            "tokenizer_http_calls": sum(e.get("tokenizer_http_calls", 0) for e in receipts)}
    if background:
        costs["t1_retrieval"]["charged_summary_background"] = {
            key: value for key, value in background[-1].items() if key.startswith("brief_")}
    costs["shared_archive_embedding_index_setup"] = {
        "charging": "shared retrieval-only/T1 index setup, not hidden or charged to oracle/recent baseline",
        "calls": background[-1].get("embedding_index_calls") if background else None,
        "seconds": background[-1].get("embedding_index_seconds") if background else None,
        "embedding_model_token_usage": "excluded: production client provides no receipt"}
    candidate = arms["t1_retrieval"]
    paired = len({(row["task"], row["arm"]) for row in scored}) == len(taskmap) * 4 == len(scored)
    gates = {
        "driver_complete": driver_rc == 0 and bool(complete) and complete[-1].get("driver_ok") is True,
        "all_tasks_all_arms_once": paired,
        "all_generation_usage_matched": bool(scored) and all(r["matched_runtime_ok"] for r in scored),
        "candidate_correctness_at_least_0_9": candidate["correctness_and_abstention"] >= .9,
        "oracle_correctness_at_least_0_9": arms["oracle_source"]["correctness_and_abstention"] >= .9,
        "candidate_quote_id_precision_1_0": candidate["source_evidence_exact_quote_id_precision"] == 1.0,
        "all_emitted_evidence_well_formed": bool(scored) and all(r["evidence_shape_ok"] for r in scored),
        "privacy_zero": bool(scored) and all(r["privacy_violations"] == 0 for r in scored),
        "brief_frontier_complete": bool(background) and background[-1].get("brief_frontier_complete") is True,
        "real_semantic_and_lexical_selection": all(e.get("semantic_ok") and e.get("lexical_ok")
            for e in answers if e.get("arm") in ("retrieval_only", "t1_retrieval")) and paired,
    }
    # A passing smoke is never promoted to a heldout-quality claim.
    return {"protocol": "memory-quality-v2", "split": fixture["split"],
            "result": "PASS" if all(gates.values()) else "FAIL", "gates": gates,
            "not_an_agreed_SLA": True, "heldout_claim_eligible": fixture["split"] == "heldout",
            "manifest_sha256": manifest, "arms": arms, "per_task": scored,
            "cost_accounting": costs,
            "paired_candidate_minus_retrieval_correct": candidate["correct"] - arms["retrieval_only"]["correct"],
            "coverage": {
                "t1": "production planner selection and memory_plan_apply/reference renderer; brief input pointer bodies NEVER added to prompt",
                "t2": "not_run", "rolling_summary": "not_run", "sustained_hours": "not_run",
                "archive": "synthetic manuscript as user-author chat originals, not canon",
                "privacy": "indexed other-conversation canary + admitted originals checked against same archive",
                "semantic_entailment_review": "excluded: exact quotes are not a semantic-fidelity proof",
                "native_chat_UI_latency_remote_switch_restart_saved_revisions": "not_run",
                "service_RSS_and_Qdrant_disk": "excluded: not sampled",
                "background_full_usage": "real brief usage_known/prompt/completion tokens, generation_ms and tokenizer_http_calls recorded per attempt and aggregated; unknown attempts explicit; embedding usage remains excluded",
                "tokenization": "exact real foreground/source counts, tokenizer HTTP calls and wall time; brief internal HTTP calls separately reported; embedding internal tokenization excluded",
                "retries": "zero driver retries; genuine generation failures retained; no tuning",
            }, "native_complete": complete[-1] if complete else None,
            "background": background[-1] if background else None}


def command_output(args):
    try:
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=20)
        return {"returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"excluded": str(error)}


def self_test():
    fixture = json.loads((FIXTURES / "dev.json").read_text())
    archive, _, tasks, words = corpus(fixture, 114207, 10000)
    assert words >= 10000 and len(tasks) == 12
    task = {"id": "test", "answers": ["625"], "required_sequences": [0]}
    original = [{"text": "The weight is 625 grams.", "sequence": 0}]
    event = {"arm": "oracle_source", "transport_ok": True, "source_tokens": 40,
             "production_reference": "unit=a\nThe weight is 625 grams.",
             "final_prompt_tokens": 100, "generator_usage": {"prompt_tokens": 100, "completion_tokens": 40},
             "admitted": {"originals": [{"id": "a", "sequence": 0, "text": original[0]["text"]}]}}
    def grade(answer):
        return score_answer(task, dict(event, response=json.dumps(answer)), original)
    good = {"status": "answer", "answer": "625", "evidence": [{"id": "a", "quote": "625 grams"}]}
    assert grade(good)["correct"] and grade(good)["citation_valid"] == 1
    assert not grade(dict(good, answer="625 or 731"))["correct"]
    assert not grade(dict(good, answer="unknown, but 625"))["correct"]
    assert not grade(dict(good, evidence=[{"id": "wrong", "quote": "625 grams"}]))["evidence_shape_ok"]
    assert not grade(dict(good, evidence=[{"id": "a", "quote": "626 grams"}]))["evidence_shape_ok"]
    pointer_event = dict(event, production_reference="brief=b ORIGINAL unit=a", admitted={
        "originals": [], "brief_input_pointers": [{"id": "a", "brief_id": "b", "sequence": 0,
            "byte_start": 0, "text_bytes": len(original[0]["text"].encode()),
            "text_hash": hashlib.sha256(original[0]["text"].encode()).hexdigest()}]}, response=json.dumps(good))
    pointer_score = score_answer(task, pointer_event, original)
    assert pointer_score["correct"] and pointer_score["source_recall"] == 0
    assert pointer_score["pointer_only_provenance_coverage"] == 1 and pointer_score["pointer_only_citations"] == 1
    assert original[0]["text"] not in pointer_event["production_reference"]
    task["abstain"] = True
    assert not grade(dict(good, status="abstain", answer="unknown, probably 625"))["correct"]
    assert grade({"status": "abstain", "answer": "", "evidence": []})["correct"]
    heldout = json.loads((FIXTURES / "heldout.json").read_text())
    _, _, heldout_tasks, heldout_words = corpus(heldout, 98147311, 100000)
    assert len(heldout_tasks) == 20 and heldout_words >= 100000
    registration = json.loads((FIXTURES / "preregistration.json").read_text())
    archived = json.loads((FIXTURES / "preregistration-v1.json").read_text())
    assert registration["answer_prompt"] == archived["answer_prompt"]
    assert registration["gates"] == archived["gates"]
    print("Scorer/corpus self-tests passed; NO MODEL QUALITY WAS TESTED.")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--heldout", action="store_true")
    parser.add_argument("--words", type=int)
    parser.add_argument("--brief-limit", type=int, default=0, help="0=full frontier; cap marks coverage incomplete and fails gate")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--snapshot-build", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/memory-quality/memory_quality_eval")
    args = parser.parse_args()
    os.environ.setdefault("SCRIBODO_LLAMA_SERVER", str(ROOT / "build/llama/bin/llama-server"))
    if args.snapshot_build:
        executable, _ = snapshot_build(args.binary.resolve())
        print(f"Frozen build artifact: {executable}")
        return 0
    if args.self_test:
        self_test()
        return 0
    words = args.words if args.words is not None else (100000 if args.heldout else 10000)
    if words < (100000 if args.heldout else 10000) or words > 300000 or args.brief_limit < 0:
        parser.error("words must be 10000..300000 (heldout >=100000); brief-limit >=0")
    split = "heldout" if args.heldout else "dev"
    registration = json.loads((FIXTURES / "preregistration.json").read_text())
    fixture = json.loads((FIXTURES / f"{split}.json").read_text())
    seed = registration["heldout_seed" if args.heldout else "dev_seed"]
    archive, recent, tasks, actual_words = corpus(fixture, seed, words)
    name = f"scribodo_test_quality_{os.getpid()}_{secrets.token_hex(8)}"
    run = ROOT / "build/memory-quality/runs" / name
    run.mkdir(parents=True, mode=0o700)
    os.chmod(run, 0o700)
    (run / "owned-database").write_text(name)
    native_input = {"protocol": registration["protocol"], "archive": archive, "recent": recent,
                    "tasks": tasks, "instructions": registration["answer_prompt"],
                    "brief_limit": args.brief_limit,
                    "private_canary": "PRIVATE-OTHER-CONVERSATION-9b174c6f Zoë Íñigo Kestrel archive key birthday 999991"}
    save(run / "input.json", native_input)
    save(run / "answer-key.json", fixture)  # Never passed to model/native driver.
    save(run / "preregistration.json", registration)
    sources = [Path(__file__), ROOT / "tools/memory_quality_eval.c", ROOT / "tools/memory_quality_eval.sh"]
    sources += sorted(FIXTURES.glob("*.json"))
    binary = args.binary.resolve()
    owned_binary = run / "memory_quality_eval"
    build_record = None
    artifact_dir = None
    if binary.is_file():
        index_path = ROOT / "build/memory-quality/build-manifest.json"
        index = json.loads(index_path.read_text()) if index_path.is_file() else {}
        if index.get("binary_sha256") == sha(binary):
            artifact_dir = Path(index["artifact"])
            artifact_binary = artifact_dir / "memory_quality_eval"
            build_manifest = artifact_dir / "build-manifest.json"
        else:
            artifact_binary, build_manifest = snapshot_build(binary)
            artifact_dir = build_manifest.parent
        build_record = json.loads(build_manifest.read_text())
        shutil.copyfile(artifact_binary, owned_binary)
        owned_binary.chmod(0o500)
        shutil.copyfile(build_manifest, run / "build-manifest.json")
    scoring_snapshot = "\n\n".join(inspect.getsource(fn) for fn in
                                      (normalized, strict_json, score_answer, summarize))
    (run / "scoring.py").write_text(scoring_snapshot)
    for source in sources:
        target = run / "frozen-sources" / source.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    llama_binary = Path(os.environ["SCRIBODO_LLAMA_SERVER"])
    if not llama_binary.is_absolute():
        llama_binary = ROOT / llama_binary
    manifest = {"protocol": registration["protocol"], "frozen_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "split": split, "seed": seed, "requested_words": words, "actual_words": actual_words,
                "task_count": len(tasks), "source_budget": 4096, "effective_window": 8192,
                "source_budget_kind": "optional native-rendered reference only; same complete recent64 tail plus question in every arm",
                "output_allowance": 128, "temperature": 0, "brief_limit": args.brief_limit,
                "database": name, "input_sha256": sha(run / "input.json"),
                "corpus_sha256": hashlib.sha256(json.dumps(archive, ensure_ascii=False, sort_keys=True).encode()).hexdigest(),
                "answer_key_sha256": sha(run / "answer-key.json"),
                "source_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sources},
                "fixture_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sorted(FIXTURES.glob("*.json"))},
                "scoring_sha256": sha(run / "scoring.py"),
                "registration_sha256": sha(run / "preregistration.json"),
                "native_binary_sha256": sha(owned_binary) if owned_binary.is_file() else None,
                "executed_artifact": str(owned_binary),
                "build_manifest_sha256": sha(run / "build-manifest.json") if build_record else None,
                "build_artifact_directory": str(artifact_dir) if artifact_dir else None,
                "llama_binary_sha256": sha(llama_binary) if llama_binary.is_file() else None,
                "llama_binary_version": command_output([str(llama_binary), "--version"]),
                "git_head": command_output(["git", "rev-parse", "HEAD"]),
                "llama_commit": command_output(["git", "-C", "third_party/llama.cpp", "rev-parse", "HEAD"]),
                "platform": command_output(["uname", "-a"]),
                "python": sys.version,
                "service_ports": {"db": os.environ.get("SCRIBODO_DB_PORT", "33061"),
                                  "qdrant": os.environ.get("SCRIBODO_QDRANT_PORT", "6333")},
                "configuration": "Pinned supervisor defaults; private run/cache/log directories; chat one 8192-token slot; Qwen embedder defaults; no author service changes."}
    save(run / "manifest.json", manifest)
    manifest_hash = sha(run / "manifest.json")
    (run / "FROZEN.sha256").write_text(manifest_hash + "\n")
    print(f"Frozen {split} run: {run}\nmanifest_sha256={manifest_hash}", flush=True)
    if args.prepare_only:
        print("Prepared only: NOT RUN. No database, vector collection or model process created.")
        return 0
    if not owned_binary.is_file():
        parser.error("native binary missing; run tools/memory_quality_eval.sh")
    for var in ("SCRIBODO_DB_PORT", "SCRIBODO_QDRANT_PORT"):
        value = os.environ.get(var)
        if value is not None and (not re.fullmatch(r"[0-9]{1,5}", value) or not 1 <= int(value) <= 65535):
            parser.error(f"invalid {var}")
    if os.environ.get("SCRIBODO_LLM") == "fake":
        parser.error("fake model selection forbidden")
    env = dict(os.environ, SCRIBODO_DB_NAME=name, SCRIBODO_MEMORY_BRIEFS="1")
    env.pop("SCRIBODO_LLM", None)
    services = ROOT / "tools/services.sh"
    created = False
    driver_rc = 1
    cleanup_rc = None
    started = time.monotonic()
    try:
        # No services up/down/reconfiguration; use existing local DB/Qdrant.
        created = True  # Attempt exact-name cleanup even if create partly fails.
        subprocess.run([str(services), "testdb", "create", name], cwd=ROOT, env=env, check=True)
        with (run / "driver.stdout.log").open("w") as out, (run / "driver.stderr.log").open("w") as err:
            driver_rc = subprocess.run(["/usr/bin/caffeinate", "-i", str(owned_binary), name, str(run), str(run / "input.json"), str(run / "events.jsonl")],
                                       cwd=ROOT, env=env, stdout=out, stderr=err).returncode
    except (OSError, subprocess.CalledProcessError, KeyboardInterrupt) as error:
        (run / "orchestration-error.txt").write_text(str(error) + "\n")
    finally:
        if created:
            cleanup_rc = subprocess.run([str(services), "testdb", "drop", name], cwd=ROOT, env=env).returncode
    events = []
    event_path = run / "events.jsonl"
    if event_path.exists():
        for line in event_path.read_text().splitlines():
            try:
                events.append(json.loads(line))
            except ValueError:
                events.append({"event": "malformed_native_event", "line": line})
    report = summarize(manifest_hash, fixture, native_input, events, driver_rc)
    report["actual_wall_seconds"] = time.monotonic() - started
    report["database_cleanup_returncode"] = cleanup_rc
    report["retained_run_disk_bytes"] = sum(p.stat().st_size for p in run.rglob("*") if p.is_file())
    report["database_disk_bytes"] = "excluded: generated DB dropped after run"
    report["retention"] = "Exact generated project/cache/logs/prompts/responses retained here; no broad filesystem cleanup."
    if cleanup_rc != 0:
        report["result"] = "FAIL"
    # Preregistered experiment data/scoring/config are separate from runtime
    # implementation provenance. Parent edits/relinks do not change our owned
    # executable, loaded scorer, input, or model prompt.
    frozen_intact = (sha(run / "manifest.json") == manifest_hash and
                     sha(run / "input.json") == manifest["input_sha256"] and
                     sha(run / "answer-key.json") == manifest["answer_key_sha256"] and
                     sha(run / "preregistration.json") == manifest["registration_sha256"] and
                     sha(run / "scoring.py") == manifest["scoring_sha256"] and
                     all((ROOT / p).is_file() and sha(ROOT / p) == value
                         for p, value in manifest["fixture_sha256"].items()))
    report["preregistration_intact"] = frozen_intact
    artifact_intact = (sha(owned_binary) == manifest["native_binary_sha256"] and
                       sha(run / "build-manifest.json") == manifest["build_manifest_sha256"] and
                       all((run / "frozen-sources" / p).is_file() and
                           sha(run / "frozen-sources" / p) == value
                           for p, value in manifest["source_sha256"].items()) and
                       all((artifact_dir / "sources" / p).is_file() and
                           sha(artifact_dir / "sources" / p) == value
                           for p, value in build_record["implementation_source_sha256"].items()))
    working_matches = all((ROOT / p).is_file() and sha(ROOT / p) == value
                          for p, value in build_record["implementation_source_sha256"].items())
    report["implementation_artifact_intact"] = artifact_intact
    report["implementation_matches_build"] = working_matches
    report["implementation_working_tree_status"] = "matches_build" if working_matches else "workingtreechangedafterbuild"
    report["root_build_binary_still_matches"] = binary.is_file() and sha(binary) == manifest["native_binary_sha256"]
    report["llama_binary_still_matches_launch_snapshot"] = llama_binary.is_file() and sha(llama_binary) == manifest["llama_binary_sha256"]
    if not frozen_intact or not artifact_intact:
        report["result"] = "FAIL"
    save(run / "report.json", report)
    print(f"{report['result']} ({split}; experimental gates, no SLA): {run / 'report.json'}")
    print(json.dumps(report["arms"], ensure_ascii=False, indent=2))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
