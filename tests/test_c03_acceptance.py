"""C03 acceptance-oracle negative controls; synthetic observations, no model loads."""
import copy
import hashlib
import importlib.util
import io
import json
from contextlib import redirect_stdout
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "c03_probe", ROOT / "journal/composition001/c03/compatibility_probe.py")
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


def fixture():
    vector = [-float(i + 1) for i in range(20)] + [-1000.0] * (128000 - 20)
    top = [{"id": i, "logprob": vector[i]} for i in range(20)]
    reply = {"completion_probabilities": [{"top_logprobs": top}]}
    meta = {"probe_sha256": "a" * 64,
            "corpus_sha256": hashlib.sha256(probe.legacy.corpus_text().encode()).hexdigest(),
            "reference_revision": probe.base.REV, "gguf_revision": probe.base.GGUF_REV,
            "packages": {"torch": "2.15.0", "transformers": "5.22.1"}}
    ids = [1] * 4096
    short = {"param_dtypes": ["torch.float32"],
             "prompts": [{"ids": [1], "logprobs": vector, "greedy": [0] * 40} for _ in range(4)]}
    long = {"positions": probe.legacy.POSITIONS, "ids": ids, "n": 4096, "dtype": "float32",
            "logprobs": [vector] * 10, "metadata": meta}
    runtime = {"metadata": meta, "gguf_sha256": probe.GGUF_SHA, "flags": probe.FLAGS,
               "short": [{"ids": [1], "own_tokenization": [1], "reply": reply, "greedy": [0] * 40}
                         for _ in range(4)],
               "long_ids": ids, "long": [{"position": p, "reply": reply} for p in probe.legacy.POSITIONS],
               "templates": [{"name": ("history after production reasoning removal"
                                      if name == "history with thinking" else name),
                              "expected": "same", "actual": "same", "identical": True}
                             for name, _, _ in probe.legacy.CONVERSATIONS],
               "tokenize": [{"text": text, "expected": [1], "actual": [1], "identical": True}
                            for text in [probe.legacy.corpus_text(), *probe.UNICODE]]}
    data = {"reference/reference.json": short, "reference/longctx_ref.json": long,
            "reference/reference-metadata.json": meta,
            "reference/identity.json": {"exit": 0, "gguf_sha256": probe.GGUF_SHA, "metadata": meta},
            "baseline/runtime.json": copy.deepcopy(runtime), "candidate/runtime.json": copy.deepcopy(runtime)}
    for label, commit, sha in (("baseline", "5851a8e50", "c" * 64), ("candidate", "03813c56e", "d" * 64)):
        data[label + "/runtime.json"].update(server_sha256=sha, server_version="commit " + commit)
    return data


class AcceptanceTests(unittest.TestCase):
    def compare(self, data):
        def load(path, *args, **kwargs):
            return json.dumps(data[str(path.relative_to(Path("/synthetic")))])
        with patch.object(Path, "read_text", load), patch.object(probe, "save"), redirect_stdout(io.StringIO()):
            return probe.compare(Path("/synthetic"))

    def refused(self, mutate):
        data = fixture()
        mutate(data)
        try:
            result = self.compare(data)
        except (RuntimeError, ValueError, KeyError, TypeError):
            return
        self.assertNotEqual(result, 0, "mutated evidence was falsely accepted")

    def test_complete_fixture(self):
        self.assertEqual(self.compare(fixture()), 0)

    def test_empty_reference_and_runtime_are_not_a_pass(self):
        def change(d):
            d["reference/reference.json"]["prompts"] = []
            d["reference/longctx_ref.json"].update(positions=[], logprobs=[])
            for label in ("baseline", "candidate"):
                d[label + "/runtime.json"].update(short=[], long=[])
        self.refused(change)

    def test_truncated_long_reference_is_not_a_pass(self):
        self.refused(lambda d: d["reference/longctx_ref.json"].update(logprobs=[]))

    def test_reference_nan_is_refused(self):
        self.refused(lambda d: d["reference/reference.json"]["prompts"][0]["logprobs"].__setitem__(0, float("nan")))

    def test_identity_failure_is_refused(self):
        self.refused(lambda d: d["reference/identity.json"].update(exit=1))

    def test_f16_kv_is_refused(self):
        def change(d):
            flags = list(d["candidate/runtime.json"]["flags"])
            flags[flags.index("-ctk") + 1] = "f16"
            d["candidate/runtime.json"]["flags"] = flags
        self.refused(change)

    def test_wrong_artifact_hash_is_refused(self):
        self.refused(lambda d: d["candidate/runtime.json"].update(gguf_sha256="0" * 64))

    def test_mixed_provenance_is_refused(self):
        def change(d):
            d["candidate/runtime.json"]["metadata"] = {**d["candidate/runtime.json"]["metadata"],
                                                     "probe_sha256": "f" * 64}
        self.refused(change)

    def test_same_binary_mislabeled_is_refused(self):
        self.refused(lambda d: d["candidate/runtime.json"].update(
            server_sha256=d["baseline/runtime.json"]["server_sha256"]))

    def test_false_template_boolean_cannot_hide_mismatch(self):
        self.refused(lambda d: d["candidate/runtime.json"]["templates"][0].update(actual="different"))

    def test_false_tokenizer_boolean_cannot_hide_mismatch(self):
        self.refused(lambda d: d["candidate/runtime.json"]["tokenize"][0].update(actual=[2]))

    def test_duplicate_template_cases_are_refused(self):
        def change(d):
            d["candidate/runtime.json"]["templates"] = [d["candidate/runtime.json"]["templates"][0]] * 5
        self.refused(change)

    def test_missing_greedy_observations_are_refused(self):
        self.refused(lambda d: d["candidate/runtime.json"]["short"][0].update(greedy=[]))

    def test_negative_token_id_is_refused(self):
        self.refused(lambda d: d["candidate/runtime.json"]["short"][0]["reply"]
                     ["completion_probabilities"][0]["top_logprobs"][19].update(id=-1))

    def test_reference_precision_is_refused(self):
        self.refused(lambda d: d["reference/reference.json"].update(param_dtypes=["torch.bfloat16"]))

    def test_substituted_template_is_refused(self):
        self.refused(lambda d: d["candidate/runtime.json"]["templates"][0].update(name="substitute"))

    def test_substituted_tokenizer_case_is_refused(self):
        self.refused(lambda d: d["candidate/runtime.json"]["tokenize"][0].update(text="substitute"))

    def test_short_gap_limit_is_strict(self):
        self.refused(lambda d: d["candidate/runtime.json"]["short"][0]["reply"]
                     ["completion_probabilities"][0]["top_logprobs"][0].update(logprob=-0.94))

    def test_missing_reference_top_ten_is_refused(self):
        self.refused(lambda d: d["candidate/runtime.json"]["short"][0]["reply"]
                     ["completion_probabilities"][0]["top_logprobs"][0].update(id=25))

    def test_existing_report_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            path.write_text("original")
            with self.assertRaises(FileExistsError):
                probe.save(path, {"pass": True})
            self.assertEqual(path.read_text(), "original")


class OutputIsolationTests(unittest.TestCase):
    def test_capture_body_limit(self):
        from unittest.mock import Mock
        spec = importlib.util.spec_from_file_location(
            "c03_capture", ROOT / "journal/composition001/c03/capture_primary_sources.py")
        capture = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(capture)
        response = io.BytesIO(b"x" * 1025)
        response.headers = {}
        with tempfile.TemporaryDirectory() as directory:
            capture.OUT = Path(directory)
            with patch.object(capture, "MAX_BODY_BYTES", 1024, create=True), \
                    patch.object(capture.urllib.request, "urlopen", return_value=response):
                with self.assertRaises(RuntimeError):
                    capture.capture("https://example.test/public", "public.json")
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_startup_failure_stops_owned_child_and_removes_key(self):
        from unittest.mock import Mock
        process = Mock()
        process.poll.return_value = None
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            private_paths = []
            original = probe.private_socket
            from contextlib import contextmanager
            @contextmanager
            def record_socket():
                with original() as sock:
                    private_paths.append(sock.parent)
                    yield sock
            with patch.object(probe, "sha", return_value=probe.GGUF_SHA), \
                    patch.object(probe, "metadata", return_value={}), \
                    patch.object(probe.base, "tokenizer", return_value=object()), \
                    patch.object(probe.subprocess, "check_output", return_value="commit 03813c56e"), \
                    patch.object(probe.subprocess, "Popen", return_value=process), \
                    patch.object(probe, "call", side_effect=RuntimeError("ordinary startup failure")) as call, \
                    patch.object(probe.time, "monotonic", side_effect=[0, 181]), \
                    patch.object(probe, "private_socket", record_socket):
                with self.assertRaises(RuntimeError):
                    probe.runtime(Path("/synthetic/server"), out)
                self.assertEqual(call.call_args.kwargs["timeout"], 1)
                process.terminate.assert_called_once()
                process.wait.assert_called_once_with(timeout=30)
            self.assertTrue(private_paths)
            self.assertFalse(private_paths[0].exists())
            self.assertFalse((out / "runtime.json").exists())

    def test_capture_refuses_existing_sources_before_network(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sentinel = root / "retrievals.json"
            sentinel.write_text("original")
            result = subprocess.run([sys.executable, str(ROOT / "journal/composition001/c03/capture_primary_sources.py"),
                                     "--out", str(root)], capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(sentinel.read_text(), "original")

    def test_runner_refuses_existing_output_before_log_truncation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            python = root / "reference-env/bin/python"
            python.parent.mkdir(parents=True)
            python.write_text("#!/bin/sh\nexit 99\n")
            python.chmod(0o700)
            out = root / "already-run"
            out.mkdir()
            sentinel = out / "driver-logs"
            sentinel.write_text("original")
            result = subprocess.run(["bash", str(ROOT / "journal/composition001/c03/run-compatibility.sh"),
                                     str(root), str(out)], cwd=ROOT, capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(sentinel.read_text(), "original")

    def test_archive_refuses_checksum_collision_before_creation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            name = "scribodo-evidence-synthetic"
            sentinel = root / (name + ".sha256")
            sentinel.write_text("original")
            result = subprocess.run([sys.executable, str(ROOT / "journal/composition001/c03/archive_parity.py"),
                                     "--run", str(root), "--candidate-build", str(root),
                                     "--name", name, "--evidence", str(root)], capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((root / (name + ".tar.gz")).exists())
            self.assertEqual(sentinel.read_text(), "original")

    def test_archive_tree_refuses_symlink(self):
        spec = importlib.util.spec_from_file_location(
            "c03_archive", ROOT / "journal/composition001/c03/archive_parity.py")
        archive = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(archive)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "original").write_text("original")
            (root / "alias").symlink_to(root / "original")
            with self.assertRaises(RuntimeError):
                list(archive.regular_tree(root))

    def test_symlink_report_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "original"
            path.write_text("original")
            alias = Path(directory) / "alias"
            alias.symlink_to(path)
            with self.assertRaises(RuntimeError):
                probe.save(alias, {"pass": True})
            self.assertEqual(path.read_text(), "original")


if __name__ == "__main__":
    unittest.main()
