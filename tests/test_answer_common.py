"""tools/answer_common.py: the helpers the answer tools share (AUDIT-004 F12)."""
import hashlib
import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import answer_common as ac  # noqa: E402


class AnswerCommonTests(unittest.TestCase):
    def test_root_is_the_repository(self):
        self.assertEqual(ac.ROOT, ROOT)
        self.assertTrue((ac.ROOT / "Makefile").is_file())

    def test_sha_matches_hashlib_across_chunks(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "f"
            data = os.urandom(3 * 1048576 + 17)
            path.write_bytes(data)
            self.assertEqual(ac.sha(path), hashlib.sha256(data).hexdigest())
            self.assertEqual(ac.sha(str(path)), hashlib.sha256(data).hexdigest())

    def test_private_socket_is_short_and_removed(self):
        with ac.private_socket() as sock:
            self.assertLess(len(str(sock).encode()), 104)
            self.assertTrue(sock.parent.is_dir())
        self.assertFalse(sock.parent.exists())

    def test_the_model_pin_matches_the_makefile(self):
        makefile = (ROOT / "Makefile").read_text()
        self.assertIn(ac.GGUF_SHA, makefile)
        self.assertIn(ac.GGUF_REV, makefile)

    def test_bedrock_probe_reexports_the_same_objects(self):
        spec = importlib.util.spec_from_file_location("answer_bedrock_probe", ROOT / "tools/answer_bedrock_probe.py")
        probe = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(probe)
        self.assertIs(probe.call, ac.call)
        self.assertIs(probe.private_socket, ac.private_socket)
        self.assertEqual(probe.SHA, ac.GGUF_SHA)
        self.assertEqual(probe.ROOT, ac.ROOT)


    def test_tools_that_hash_themselves_hash_answer_common_too(self):
        """A run that records its tool's or runner's hash records the shared helpers' too (red-team 5d)."""
        for path in sorted((ROOT / "tools").glob("answer_*.py")):
            text = path.read_text()
            if path.name == "answer_common.py" or "answer_common" not in text:
                continue
            if any(k in text for k in ("runner_sha256", "probe_sha256", "helpers_sha256", "tool_sha256")):
                with self.subTest(tool=path.name):
                    self.assertIn("answer_common.py'", text.replace('"', "'"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
