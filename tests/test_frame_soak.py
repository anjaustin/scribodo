import contextlib
import importlib.util
import io
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location("frame_soak_run", Path(__file__).resolve().parents[1] / "tools/frame_soak_run.py")
frame_soak = importlib.util.module_from_spec(spec)
spec.loader.exec_module(frame_soak)


class ReportTests(unittest.TestCase):
    def test_backend_labels_and_scope(self):
        for backend, label in (("real", "real-pinned-model"), ("fake", "fake-no-LLM")):
            with self.subTest(backend=backend):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    frame_soak.RssStats().report(42, 1.0, 0, backend)
                text = output.getvalue()
                self.assertIn("backend=" + label, text)
                self.assertIn("excludes model-server and service RSS", text)
                self.assertNotIn("fake backend/no LLM", text)
                self.assertIn("peak_rss_kib=unavailable", text)


if __name__ == "__main__":
    unittest.main()
