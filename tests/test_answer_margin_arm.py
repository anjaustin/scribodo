"""Margin arm: the deterministic parts only; no model, no server (tools/answer_margin_arm.py)."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_margin_arm', ROOT / 'tools/answer_margin_arm.py')
ma = importlib.util.module_from_spec(spec); spec.loader.exec_module(ma)

# The Kantos Kan shape (mars-pipe7): six units within 0.006 of the top, the stating unit seventh; pool order is
# the run's index order, not score order.
KANTOS = [('c7fd', 0.9953), ('e02e', 0.9971), ('f57d', 0.7294), ('5e04', 0.9958), ('4330', 0.9968), ('c300', 0.9637),
          ('87c5', 0.9896), ('f175', 0.9949), ('8226', 0.9967), ('c735', 0.9913)]


class MarginArmTests(unittest.TestCase):
    def test_tie_is_every_unit_within_the_margin_of_the_top_in_pool_order(self):
        self.assertEqual(ma.tie(KANTOS, 0.0005), ['e02e', '4330', '8226'])
        self.assertEqual(ma.tie(KANTOS, 0.002), ['c7fd', 'e02e', '5e04', '4330', '8226'])
        self.assertEqual(ma.tie(KANTOS, 0.01), ['c7fd', 'e02e', '5e04', '4330', '87c5', 'f175', '8226', 'c735'])
        self.assertEqual(ma.tie([], 0.01), [])

    def test_top_two_is_the_production_rule_and_keeps_pool_order_on_equal_scores(self):
        self.assertEqual(ma.top_two(['a', 'b', 'c'], {'a': .5, 'b': .9, 'c': .9}.get), ['b', 'c'])
        self.assertEqual(ma.top_two(['a'], {'a': .5}.get), ['a'])
        self.assertEqual(ma.top_two([], {}.get), [])

    def test_today_reproduces_the_run_and_the_embedder_alone_reorders_everything(self):
        embed = {u: i / 10 for i, (u, _) in enumerate(KANTOS)}  # the embedder prefers the last units
        self.assertEqual(ma.slots_today(KANTOS, embed), ['e02e', '4330'])
        self.assertEqual(ma.slots_embedder(KANTOS, embed), ['c735', '8226'])

    def test_margin_rule_defers_to_the_embedder_only_inside_a_tie_wider_than_the_slots(self):
        embed = {u: i / 10 for i, (u, _) in enumerate(KANTOS)}
        self.assertEqual(ma.slots_margin(KANTOS, embed, 0.0001), ['e02e', '4330'])  # tie of one: today's slots
        self.assertEqual(ma.slots_margin(KANTOS, embed, 0.0003), ['e02e', '4330'])  # tie of two: unchanged
        self.assertEqual(ma.slots_margin(KANTOS, embed, 0.01), ['c735', '8226'])    # tie of eight: the embedder's two

    def test_window_rule_uses_the_best_window_inside_the_tie_and_falls_back_to_the_unit_score(self):
        window = {'c735': 0.9999, 'e02e': 0.95, '4330': None}
        slots = ma.slots_window(KANTOS, {}, 0.01, window)
        self.assertEqual(slots[0], 'c735')
        self.assertEqual(slots[1], '4330')  # no window score: its unit score 0.9968 beats every other tied unit
        self.assertEqual(ma.slots_window(KANTOS, {}, 0.0001, window), ['e02e', '4330'])

    def test_fused_rule_is_reciprocal_rank_fusion_inside_the_tie(self):
        fused = ma.rrf(['a', 'b', 'c'], {'a': .9, 'b': .5, 'c': .1}.get, {'a': .1, 'b': .5, 'c': .9}.get, k=1)
        self.assertAlmostEqual(fused['a'], 1 / 2 + 1 / 4)
        self.assertAlmostEqual(fused['b'], 1 / 3 + 1 / 3)
        self.assertEqual(ma.RRF_K, 60)
        embed = {u: 0.0 for u, _ in KANTOS}; embed['c735'] = 1.0
        self.assertEqual(ma.slots_fused(KANTOS, embed, 0.01)[0], 'e02e')  # first in one ranking, c735 seventh in the other

    def test_name_phrase_is_the_longest_capitalized_run_after_the_first_word(self):
        self.assertEqual(ma.name_phrase('Who is Kantos Kan when the narrator first meets him in the Warhoon dungeon?'), 'Kantos Kan')
        self.assertEqual(ma.name_phrase('How old is Charlotte Lucas’s brother?'), 'Charlotte Lucas')
        self.assertEqual(ma.name_phrase('How far is the walk?'), '')

    def test_first_mention_ranks_the_unit_among_mentioning_units_in_manuscript_order(self):
        docs = [{'unit_id': 'b' * 64, 'document_name': 'part06.md', 'byte_start': 900, 'text': 'Kantos Kan fought.'},
                {'unit_id': 'a' * 64, 'document_name': 'part06.md', 'byte_start': 100, 'text': 'My fellow prisoner was Kantos Kan.'},
                {'unit_id': 'c' * 64, 'document_name': 'part05.md', 'byte_start': 5000, 'text': 'No one here.'},
                {'unit_id': 'd' * 64, 'document_name': 'part07.md', 'byte_start': 1, 'text': 'Kantos Kan again.'}]
        self.assertEqual(ma.first_mention('Kantos Kan', docs, 'a' * 12), (1, 3))
        self.assertEqual(ma.first_mention('Kantos Kan', docs, 'd' * 12), (3, 3))
        self.assertEqual(ma.first_mention('Kantos Kan', docs, 'c' * 12), (None, 3))
        self.assertEqual(ma.first_mention('', docs, 'a' * 12), (None, 0))

    def test_evaluate_counts_gains_losses_and_changes_against_today(self):
        case = {'id': 'kantos', 'label': 'run', 'pool': KANTOS, 'dropped': 0, 'designated': ['c735'], 'run_slots': ['e02e', '4330']}
        embed = {('kantos', 'run', u): i / 10 for i, (u, _) in enumerate(KANTOS)}
        rows, totals = ma.evaluate([case], embed, {('kantos', 'run', 'c735'): 32}, {('kantos', 'run', 'c735'): 0.9}, {})
        self.assertTrue(rows[0]['today_matches_run'])
        self.assertEqual(rows[0]['designated'][0]['rerank_rank'], 7)
        self.assertEqual(rows[0]['designated'][0]['embed_rank_in_pool'], 1)
        self.assertEqual(totals['today'], {'designated_in_slots': 0, 'cases_changed': 0, 'cases_lost': 0, 'cases_gained': 0})
        self.assertEqual(totals['margin_0.01'], {'designated_in_slots': 1, 'cases_changed': 1, 'cases_lost': 0, 'cases_gained': 1})
        self.assertEqual(totals['margin_0.002'], {'designated_in_slots': 0, 'cases_changed': 1, 'cases_lost': 0, 'cases_gained': 0})
        self.assertEqual(set(totals), set(ma.variants()))


if __name__ == '__main__':
    unittest.main()
