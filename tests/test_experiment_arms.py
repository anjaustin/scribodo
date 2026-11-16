"""tools/experiment_arms.py: every experiment flag pinned with its default stated, and measurements use it (AUDIT-004 F4)."""
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import experiment_arms as ea  # noqa: E402

SYNTHETIC = '''
static int positive_int(const char *name) { const char *v = getenv(name); return v ? atoi(v) : 0; }
static bool off_value(const char *v) { return !v || !strcmp(v, "off"); }
static int level_arm(void)
{
    const char *v = getenv("SCRIBODO_L");
    if (off_value(v)) return 0;
    warn_value("SCRIBODO_L", v, "off, a");
    return 1;
}

static void read_environment(void)
{
    Experiments e = {
        .a = flag_is("SCRIBODO_A", "1"),
        .b = flag_on("SCRIBODO_B"),
        .c = flag_is("SCRIBODO_C", "message"),
        .d = getenv("SCRIBODO_D") != NULL,
        .e = flag_on("SCRIBODO_E"),
        .e_req = flag_is("SCRIBODO_E", "verified"),
        .l = level_arm(),
        .h = positive_int("SCRIBODO_H"),
    };
}
'''


class ArmsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.src = Path(self.tmp.name) / "experiments.c"
        self.src.write_text(SYNTHETIC)

    def test_defaults_are_stated_as_values(self):
        self.assertEqual(ea.arms(self.src), [("SCRIBODO_L", "off"), ("SCRIBODO_A", "0"), ("SCRIBODO_B", "1"),
                                             ("SCRIBODO_C", "off"), ("SCRIBODO_D", None), ("SCRIBODO_E", "1"),
                                             ("SCRIBODO_H", None)])
        kinds = {v: k for v, k, _ in ea.classify(self.src)}
        self.assertEqual((kinds["SCRIBODO_L"], kinds["SCRIBODO_D"], kinds["SCRIBODO_H"]), ("level", "switch", "hook"))

    def test_an_unclassifiable_literal_stops_the_run(self):
        for extra in ('    const char *x = getenv("SCRIBODO_NEW");\n', '    puts("SCRIBODO_ODD");\n'):
            with self.subTest(extra=extra):
                self.src.write_text(SYNTHETIC.replace("static void read_environment(void)\n{\n",
                                                      "static void read_environment(void)\n{\n" + extra))
                with self.assertRaises(SystemExit) as cm:
                    ea.classify(self.src)
                self.assertIn("cannot classify", str(cm.exception))

    def test_a_getenv_before_a_level_helper_is_not_a_level(self):
        # Verification hole C: atoi(getenv(X)) on one line, followed by a multi-line helper using off_value, was
        # classified as a level flag and pinned off.
        text = SYNTHETIC.replace("static int level_arm(void)",
                                 'static int count(void) { return atoi(getenv("SCRIBODO_N")); }\nstatic int level_arm(void)')
        self.src.write_text(text)
        with self.assertRaises(SystemExit) as cm:
            ea.classify(self.src)
        self.assertIn("SCRIBODO_N", str(cm.exception))
        # The same getenv into a variable that a later function, not its own, tests with off_value: not a level.
        text = SYNTHETIC.replace("static int level_arm(void)", 'static int other(void)\n{\n    const char *w = getenv("SCRIBODO_W");\n'
                                 '    return w != 0;\n}\nstatic int level_arm(void)')
        self.src.write_text(text)
        with self.assertRaises(SystemExit) as cm:
            ea.classify(self.src)
        self.assertIn("SCRIBODO_W", str(cm.exception))

    def test_hooks_are_recorded_only_when_set_and_never_pinned(self):
        env = ea.pin({}, self.src)
        self.assertNotIn("SCRIBODO_H", env)
        self.assertNotIn("SCRIBODO_H", ea.record(env, self.src))
        self.assertEqual(ea.record(dict(env, SCRIBODO_H="250"), self.src)["SCRIBODO_H"], "250")
        self.assertEqual(env["SCRIBODO_L"], "off")

    def test_pin_keeps_a_callers_value_and_fills_the_rest(self):
        env = ea.pin({"SCRIBODO_E": "off", "PATH": "/bin"}, self.src)
        self.assertEqual(env["SCRIBODO_E"], "off")
        self.assertEqual(env["SCRIBODO_B"], "1")
        self.assertEqual(env["SCRIBODO_A"], "0")
        self.assertNotIn("SCRIBODO_D", env)  # any value would turn a debug switch on
        self.assertEqual(env["PATH"], "/bin")

    def test_record_and_line(self):
        env = ea.pin({}, self.src)
        self.assertEqual(ea.record(env, self.src)["SCRIBODO_D"], None)
        self.assertEqual(ea.line(env, self.src),
                         "ARMS SCRIBODO_L=off SCRIBODO_A=0 SCRIBODO_B=1 SCRIBODO_C=off SCRIBODO_D=unset SCRIBODO_E=1")

    def test_shell_export_round_trip(self):
        env = {"PATH": os.environ["PATH"], "SCRIBODO_B": "0"}
        out = subprocess.run(["bash", "-c", 'eval "$(python3 %s --export --source %s)"; python3 %s --line --source %s'
                              % (ROOT / "tools/experiment_arms.py", self.src, ROOT / "tools/experiment_arms.py", self.src)],
                             env=env, capture_output=True, text=True, check=True).stdout
        self.assertEqual(out.strip(), "ARMS SCRIBODO_L=off SCRIBODO_A=0 SCRIBODO_B=0 SCRIBODO_C=off SCRIBODO_D=unset "
                                      "SCRIBODO_E=1")


class ProductTableTests(unittest.TestCase):
    def test_every_flag_of_the_product_table_is_found(self):
        header = (ROOT / "src/core/experiments.h").read_text()
        struct = header[header.index("typedef struct"):header.index("} Experiments;")]
        fields = re.findall(r"^\s*bool\s+([a-z_]+);", struct, re.M)
        source = (ROOT / "src/core/experiments.c").read_text()
        initialised = re.findall(r"\.([a-z_]+)\s*=", source[source.index("read_environment"):])
        self.assertEqual(sorted(fields), sorted(initialised))
        names = [v for v, _ in ea.arms()]
        self.assertIn("SCRIBODO_ANSWER_PIPELINE", names)
        self.assertIn("SCRIBODO_MEMORY_EXPERIMENT", names)
        self.assertEqual(dict(ea.arms())["SCRIBODO_ANSWER_PIPELINE"], "1")  # default on since 52d44fe


class ObserverBranchTests(unittest.TestCase):
    """remed/observer reads its two observer flags in helpers above read_environment (levels) and adds a test hook."""

    def test_the_observer_branch_table_classifies(self):
        shown = subprocess.run(["git", "-C", str(ROOT), "show", "remed/observer:src/core/experiments.c"],
                               capture_output=True, text=True)
        if shown.returncode != 0:
            self.skipTest("no remed/observer branch here")
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "experiments.c"
            src.write_text(shown.stdout)
            kinds = {v: (k, dflt) for v, k, dflt in ea.classify(src)}
        self.assertEqual(kinds["SCRIBODO_OBSERVER_SENTENCES"], ("level", "off"))
        self.assertEqual(kinds["SCRIBODO_OBSERVER_CONFLICT_GATE"], ("level", "off"))
        self.assertEqual(kinds["SCRIBODO_FAKE_EXTRACT_MS"], ("hook", None))
        self.assertEqual(kinds["SCRIBODO_ANSWER_PIPELINE"], ("arm", "1"))


class MeasurementsPinTests(unittest.TestCase):
    """A measurement script that starts the app or a product-linked driver pins every arm."""

    PINNED = ["tools/manuscript_baseline.sh", "tools/manuscript_restart.sh", "tools/history_frame_soak.sh",
              "tools/memory_pipeline_soak.sh", "tools/frame_soak_run.py", "tools/memory_reach_eval.py",
              "tools/memory_quality_eval.py", "tools/memory_native_eval.py", "tools/memory_operation_eval.py",
              "tools/answer_product_run.py", "tools/answer_gate_replay.py"]

    def test_listed_measurements_use_the_pin(self):
        for path in self.PINNED:
            with self.subTest(path=path):
                self.assertTrue("experiment_arms" in (ROOT / path).read_text(), "%s does not pin the arms" % path)

    def test_no_script_sets_an_arm_without_the_pin(self):
        flags = [v for v, _ in ea.arms()]
        for path in sorted((ROOT / "tools").glob("*")):
            if not path.is_file() or path.suffix not in (".sh", ".py") or path.name.startswith(("test_", "observer_")):
                continue  # mutation controls set no arm; observer_* belong to the observer stream (F4, theirs)
            if path.name == "experiment_arms.py":
                continue
            text = path.read_text()
            if path.suffix == ".sh":
                code = "\n".join(l for l in text.splitlines() if not l.lstrip().startswith(("#", "echo")))
                sets = any(re.search(r"(^|\s)%s=" % f, code, re.M) for f in flags)
            else:  # a dict entry or keyword argument, not prose in a docstring
                sets = any(re.search(r"[\'\"]%s[\'\"]\s*:|\b%s=[\'\"]" % (f, f), text) for f in flags)
            if sets:
                with self.subTest(path=path.name):
                    self.assertTrue("experiment_arms" in text, "%s sets an experiment flag but does not pin the rest"
                                    % path.name)

if __name__ == "__main__":
    unittest.main(verbosity=2)
