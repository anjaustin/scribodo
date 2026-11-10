"""Ranker instruction comparison: pool reconstruction, rank metrics, fixed instructions; no model."""
import importlib.util
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_ranker_instruction', ROOT / 'tools/answer_ranker_instruction.py')
ri = importlib.util.module_from_spec(spec); spec.loader.exec_module(ri)


def record():
    return {'cases': [
        {'id': 'a', 'units': [
            {'label': 'part01.md', 'span': 'stating sentence', 'unit_id': 'u1', 'added': False, 'expected_answers': True},
            {'label': 'part02.md', 'span': 'other sentence', 'unit_id': 'u2', 'added': False, 'expected_answers': False},
            {'label': 'part03.md', 'span': None, 'unit_id': 'u3', 'added': False, 'expected_answers': False},
            {'label': 'part04.md', 'span': 'added sentence', 'unit_id': 'u4', 'added': True, 'expected_answers': True}]},
        {'id': 'b', 'units': [{'label': 'part01.md', 'span': 'only', 'unit_id': 'u5', 'added': False, 'expected_answers': False}]}]}


class RankerInstructionTests(unittest.TestCase):
    def test_pools_keep_only_verified_spans_and_split_added(self):
        pools = ri.pools_from_record(record())
        self.assertEqual([s[2] for s in pools['a']['realistic']], ['u1', 'u2'])
        self.assertEqual([s[2] for s in pools['a']['added']], ['u1', 'u2', 'u4'])
        self.assertEqual(pools['b']['added'], [])

    def test_rank_metrics(self):
        items = [{'expected': False}, {'expected': True}, {'expected': False}]
        rows = ri.ranked([1.0, 0.0], [[0.0, 1.0], [0.8, 0.6], [1.0, 0.0]], items)
        self.assertEqual([r['rank'] for r in rows], [1, 2, 3])
        self.assertEqual([r['expected'] for r in rows], [False, True, False])
        exp = ri.expected_ranks(rows)
        self.assertEqual(exp[0]['rank'], 2); self.assertEqual(exp[0]['of'], 3)
        self.assertAlmostEqual(exp[0]['margin_to_best_other'], 0.8 - 1.0, places=6)
        self.assertEqual(ri.mrr([{'rank': 1}, {'rank': 2}, {'rank': 4}]), round((1 + .5 + .25) / 3, 4))
        self.assertIsNone(ri.mrr([]))

    def test_instructions_are_fixed_and_production_matches_the_source(self):
        source = (ROOT / 'src/llm/semantic.c').read_text()
        m = re.search(r'k_query_instruction =\s*((?:"[^"]*"\s*)+);', source)
        self.assertIsNotNone(m)
        literal = ''.join(re.findall(r'"([^"]*)"', m.group(1))).encode().decode('unicode_escape')
        self.assertEqual(ri.INSTRUCTIONS['production'], literal)
        self.assertEqual(set(ri.INSTRUCTIONS), {'production', 'question', 'qwen_default', 'none'})
        self.assertEqual(ri.INSTRUCTIONS['none'], '')
        for name in ('production', 'question', 'qwen_default'):
            self.assertTrue(ri.INSTRUCTIONS[name].startswith('Instruct: ') and ri.INSTRUCTIONS[name].endswith('\nQuery: '), name)


if __name__ == '__main__':
    unittest.main()
