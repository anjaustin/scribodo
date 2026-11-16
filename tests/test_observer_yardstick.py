"""tools/observer_yardstick.py: the comparison and the skip rule, without running the replay."""
import importlib.util
import os
import sys
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("observer_yardstick", os.path.join(ROOT, "tools/observer_yardstick.py"))
yard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(yard)


def rec(lines, sha, inputs="in"):
    return {"lines": lines, "sha256": sha, "inputs_sha256": inputs}


class YardstickTests(unittest.TestCase):
    def setUp(self):
        self.expected = {"novels": {"pp": rec(717, "a"), "mars": rec(348, "b"), "heights": rec(948, "c")}}

    def test_identical_run_passes(self):
        got = {"pp": rec(717, "a"), "mars": rec(348, "b"), "heights": rec(948, "c")}
        self.assertEqual(yard.compare(self.expected, got), [])

    def test_changed_output_fails_even_with_the_same_count(self):
        got = {"pp": rec(717, "z"), "mars": rec(348, "b"), "heights": rec(948, "c")}
        problems = yard.compare(self.expected, got)
        self.assertEqual(len(problems), 1)
        self.assertTrue(problems[0].startswith("pp: 717 lines"))

    def test_changed_inputs_are_reported_as_inputs_not_a_regression(self):
        got = {"pp": rec(700, "z", inputs="other"), "mars": rec(348, "b"), "heights": rec(948, "c")}
        problems = yard.compare(self.expected, got)
        self.assertIn("input chapters differ", problems[0])

    def test_a_missing_novel_fails(self):
        got = {"pp": rec(717, "a"), "mars": rec(348, "b")}
        self.assertTrue(any(p.startswith("heights: missing") for p in yard.compare(self.expected, got)))

    def test_skip_needs_a_hosted_runner_or_the_explicit_variable(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertFalse(yard.may_skip())
        with mock.patch.dict(os.environ, {"CI": "true"}, clear=True):
            self.assertFalse(yard.may_skip())  # the self-hosted runner sets CI too
        with mock.patch.dict(os.environ, {"CI": "true", "RUNNER_ENVIRONMENT": "github-hosted"}, clear=True):
            self.assertTrue(yard.may_skip())
        with mock.patch.dict(os.environ, {"SCRIBODO_YARDSTICK_SKIP": "1"}, clear=True):
            self.assertTrue(yard.may_skip())

    def test_the_tracked_record_holds_the_journaled_counts(self):
        import json
        with open(os.path.join(ROOT, "tests/fixtures/observer_yardstick.json"), encoding="utf-8") as f:
            novels = json.load(f)["novels"]
        self.assertEqual({n: novels[n]["lines"] for n in ("pp", "mars", "heights")},
                         {"pp": 717, "mars": 348, "heights": 948})


if __name__ == "__main__":
    unittest.main(verbosity=2)
