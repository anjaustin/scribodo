"""Admission arm: deterministic parts only; no model, no server."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_admission_arm', ROOT / 'tools/answer_admission_arm.py')
aa = importlib.util.module_from_spec(spec); spec.loader.exec_module(aa)


class AdmissionArmTests(unittest.TestCase):
    def test_context_parts_carry_the_request_context_and_leave_one_part_requests_alone(self):
        self.assertEqual(aa.context_parts('My drafts disagree about the miles. Which figure does the manuscript give, and what did the other say?'),
                         ['My drafts disagree about the miles. Which figure does the manuscript give?',
                          'My drafts disagree about the miles. what did the other say?'])
        self.assertEqual(aa.context_parts('How far is the walk?'), ['How far is the walk?'])
        q = 'Draft two sentences. Mark it as new writing.'
        self.assertEqual(aa.context_parts(q), [q])

    def test_top_k_and_union_pool(self):
        self.assertEqual(aa.top_k([0.1, 0.9, 0.9, 0.5], 2), [1, 2])
        self.assertEqual(aa.union_pool([0.1, 0.9, 0.9, 0.5], [0.9, 0.0, 0.0, 0.8], 2, False), [1, 2])
        self.assertEqual(aa.union_pool([0.1, 0.9, 0.9, 0.5], [0.9, 0.0, 0.0, 0.8], 2, True), [1, 2, 0, 3])

    def test_matching_prefers_the_pair_with_the_higher_total_over_the_greedy_first_choice(self):
        # Part 1's pool scores unit 0 (Bingley) 0.98 and unit 1 (sisters) 0.97; part 2 scores unit 0 0.99 and unit 3 0.74.
        score = {(0, 0): .98, (0, 1): .97, (0, 2): .5, (1, 0): .99, (1, 3): .74}
        self.assertEqual(aa.matching([[0, 1, 2], [0, 3]], lambda p, u: score[(p, u)]), [1, 0])
        self.assertEqual(aa.greedy([0, 1, 2], lambda u: score[(0, u)]), [0, 1])

    def test_rrf_sums_reciprocal_ranks_within_the_pool(self):
        pool = [1, 2, 3]
        fused = aa.rrf(pool, lambda u: {1: .9, 2: .5, 3: .1}[u], lambda u: {1: .1, 2: .5, 3: .9}[u], k=1)
        self.assertAlmostEqual(fused[1], 1 / 2 + 1 / 4)  # first in one ranking, third in the other
        self.assertAlmostEqual(fused[2], 1 / 3 + 1 / 3)
        self.assertAlmostEqual(fused[3], 1 / 4 + 1 / 2)
        self.assertEqual(aa.RRF_K, 60)

    def test_matching_with_one_part_is_the_greedy_top_two_of_its_pool(self):
        score = {(0, 5): .2, (0, 6): .9, (0, 7): .5}
        self.assertEqual(aa.matching([[5, 6, 7]], lambda p, u: score[(p, u)]), [6, 7])

    def test_matching_fills_a_single_unit_pool_from_the_first_part(self):
        score = {(0, 1): .3, (0, 2): .8, (1, 2): .9}
        self.assertEqual(aa.matching([[1, 2], [2]], lambda p, u: score[(p, u)]), [1, 2])  # part 2 needs unit 2; part 1 takes unit 1


if __name__ == '__main__':
    unittest.main()
