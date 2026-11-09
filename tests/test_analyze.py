"""Exercise the real Make analyzer gate, including failures and coverage."""
import pathlib
import re
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class AnalyzeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="analyze-test-", dir=ROOT / "build")
        self.addCleanup(self.temp.cleanup)
        self.work = pathlib.Path(self.temp.name)
        self.source = self.work / "probe.c"

    def run_analyze(self, source="int clean(void) { return 0; }\n", analyzer=None):
        self.source.write_text(source)
        args = ["make", "--no-print-directory", "analyze",
                f"BUILD={self.work}", f"ANALYZE_SRCS={self.source}"]
        if analyzer is not None:
            args.append(f"ANALYZE={analyzer}")
        return subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=60)

    def test_default_inventory_covers_c_and_objective_c(self):
        result = subprocess.run(["make", "--no-print-directory", "-n", "analyze"],
                                cwd=ROOT, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = {p.relative_to(ROOT).as_posix() for p in (ROOT / "src").rglob("*")
                    if p.suffix in (".c", ".m")}
        expected -= {"src/ui/vendor_impl.c", "src/core/json_vendor.c"}
        actual = set(re.findall(r"\bsrc/[^\s]+\.[cm]\b", result.stdout))
        self.assertEqual(actual, expected)

    def test_clean_source_passes(self):
        result = self.run_analyze()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 warnings across 1 source files", result.stdout)
        self.assertEqual((self.work / "analyze.log").read_text(), "")

    def test_warning_fails_even_when_clang_exits_zero(self):
        result = self.run_analyze("int broken(void) { int *p = 0; return *p; }\n")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("warning:", (self.work / "analyze.log").read_text())

    def test_compile_error_fails(self):
        result = self.run_analyze("int broken( {\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("error:", (self.work / "analyze.log").read_text())

    def test_missing_analyzer_fails(self):
        result = self.run_analyze(analyzer=str(self.work / "missing-clang"))
        self.assertNotEqual(result.returncode, 0)

    def test_nonzero_analyzer_without_diagnostics_fails(self):
        result = self.run_analyze(analyzer="false")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.work / "analyze.log").read_text(), "")

    def test_nonwarning_diagnostic_fails(self):
        analyzer = self.work / "diagnostic-clang"
        analyzer.write_text("#!/bin/sh\necho 'note: diagnostic probe' >&2\nexit 0\n")
        analyzer.chmod(0o700)
        result = self.run_analyze(analyzer=str(analyzer))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("note: diagnostic probe", result.stdout)

    def test_clean_rerun_discards_stale_findings(self):
        result = self.run_analyze("int broken(void) { int *p = 0; return *p; }\n")
        self.assertNotEqual(result.returncode, 0)
        result = self.run_analyze()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.work / "analyze.log").read_text(), "")


if __name__ == "__main__":
    unittest.main()
