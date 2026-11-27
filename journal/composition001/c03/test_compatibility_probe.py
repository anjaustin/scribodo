"""Toy numerical checks of the comparison oracle; no model/server execution."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("c03_compatibility", Path(__file__).with_name("compatibility_probe.py"))
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class DistributionTests(unittest.TestCase):
    def setUp(self):
        self.reference = [-float(i + 1) for i in range(20)]
        self.top = [{"id": i, "logprob": value} for i, value in enumerate(self.reference)]

    def test_matching_distribution(self):
        self.assertEqual(probe.gap(self.reference, self.top), {"covered": True, "max_gap": 0.0})

    def test_reference_top_ten_not_candidate_order(self):
        top = list(reversed(self.top))
        top[-1] = {"id": 0, "logprob": self.reference[0] + 0.125}
        self.assertEqual(probe.gap(self.reference, top), {"covered": True, "max_gap": 0.125})

    def test_missing_reference_token_is_not_covered(self):
        self.assertEqual(probe.gap(self.reference, self.top[1:]), {"covered": False, "max_gap": None})

    def test_duplicate_candidate_ids_are_refused(self):
        with self.assertRaises(RuntimeError):
            probe.gap(self.reference, [*self.top, self.top[0]])

    def test_nonfinite_candidate_probabilities_are_refused(self):
        for value in (float("nan"), float("inf"), -float("inf")):
            with self.assertRaises(RuntimeError):
                probe.gap(self.reference, [{"id": 0, "logprob": value}])


if __name__ == "__main__":
    unittest.main()
