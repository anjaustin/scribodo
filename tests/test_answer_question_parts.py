"""Per-part reranking arm: deterministic parts only; no model, no server."""
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_question_parts', ROOT / 'tools/answer_question_parts.py')
qp = importlib.util.module_from_spec(spec); spec.loader.exec_module(qp)
FIXTURE = json.loads((ROOT / 'tests/fixtures/answer_boundary/questions.json').read_text())


class QuestionPartsTests(unittest.TestCase):
    def test_conjoined_wh_parts_are_split_and_each_ends_in_a_question_mark(self):
        q = "What fortune do Mr. Bingley's sisters have, and what is Mr. Bingley's own inheritance said to be?"
        self.assertEqual(qp.question_parts(q), ["What fortune do Mr. Bingley's sisters have?",
                                                "what is Mr. Bingley's own inheritance said to be?"])
        self.assertEqual(qp.question_parts('Which figure does the current manuscript give, and what did the other draft say?'),
                         ['Which figure does the current manuscript give?', 'what did the other draft say?'])

    def test_a_no_break_space_after_the_question_mark_ends_the_sentence(self):
        # The C splitter reads Unicode spaces as spaces (test_question_parts); the mirror's \s already did.
        self.assertEqual(qp.question_parts('Who is Jane?\u00a0Who is Bingley?'), ['Who is Jane?', 'Who is Bingley?'])

    def test_honorific_periods_do_not_end_a_sentence(self):
        q = "In my manuscript, on whom is Mr. Bennet's estate entailed, and why is that unfortunate for his daughters?"
        self.assertEqual(qp.question_parts(q), ["In my manuscript, on whom is Mr. Bennet's estate entailed?",
                                                'why is that unfortunate for his daughters?'])
        self.assertEqual(qp.question_parts('Where does Mrs. Bennet go? Why?'), ['Where does Mrs. Bennet go?', 'Why?'])

    def test_only_question_sentences_are_parts(self):
        q = 'Earlier I told you Lydia is seventeen; I was wrong, the text says otherwise. How old is my youngest Bennet daughter?'
        self.assertEqual(qp.question_parts(q), ['How old is my youngest Bennet daughter?'])

    def test_a_conjunction_without_a_wh_word_does_not_split(self):
        self.assertEqual(qp.question_parts('Does Jane walk and ride to Netherfield?'), ['Does Jane walk and ride to Netherfield?'])

    def test_a_request_with_no_question_sentence_is_one_part_unchanged(self):
        q = "Draft two sentences, in Austen's register, of Mr. Bennet privately reflecting on the entail. Mark it as new writing."
        self.assertEqual(qp.question_parts(q), [q])
        self.assertEqual(qp.question_parts('  spaced  '), ['spaced'])

    def test_every_fixture_question_splits_into_at_most_two_parts_each_ending_in_a_question_mark(self):
        for case in FIXTURE['cases']:
            parts = qp.question_parts(case['question'])
            self.assertTrue(1 <= len(parts) <= 2, case['id'])
            if case['question'].rstrip().endswith('?'):
                self.assertTrue(all(p.endswith('?') for p in parts), case['id'])

    def test_clip_cuts_at_a_character_boundary(self):
        text = 'a' * 1998 + '“' + 'tail'  # the curly quote is three bytes and straddles the limit
        clipped = qp.clip_utf8(text)
        self.assertEqual(clipped, 'a' * 1998)
        self.assertEqual(qp.clip_utf8('short'), 'short')

    def test_ranks_break_ties_by_pool_order(self):
        self.assertEqual(qp.ranks([('a', 0.5), ('b', 0.9), ('c', 0.9)]), {'b': 1, 'c': 2, 'a': 3})

    def test_slots_take_one_unit_per_part_then_fill_from_the_whole_question(self):
        full = [('a', 0.9), ('b', 0.8), ('c', 0.7)]
        self.assertEqual(qp.slots([], full), ['a', 'b'])  # today's selection
        part1 = [('a', 0.1), ('b', 0.2), ('c', 0.95)]
        part2 = [('a', 0.3), ('b', 0.1), ('c', 0.99)]
        self.assertEqual(qp.slots([part1, part2], full), ['c', 'a'])  # distinct: part 2's top is taken, so its next
        self.assertEqual(qp.slots([part1], full), ['c', 'a'])  # one part: second slot from the whole question

    def test_pools_pair_traces_to_cases_by_chosen_unit_in_reply_order(self):
        fixture = {'cases': [{'id': 'x', 'question': 'Q1?'}, {'id': 'y', 'question': 'Q2?'}, {'id': 'z', 'question': 'Q3?'}]}
        messages = [{'seq': 1, 'role': 'user', 'content': 'Q1?'},
                    {'seq': 2, 'role': 'assistant', 'memory_sources': '--- VERIFIED SOURCE | unit=aaaaaaaaaaaa00 | document=d | score=0.9 | status=verified ---\n'},
                    {'seq': 3, 'role': 'user', 'content': 'Q2?'},
                    {'seq': 4, 'role': 'assistant', 'memory_sources': '--- VERIFIED SOURCE | unit=aaaaaaaaaaaa00 | document=d | score=0.9 | status=verified ---\n'},
                    {'seq': 5, 'role': 'user', 'content': 'Q3?'},
                    {'seq': 6, 'role': 'assistant', 'memory_sources': '--- VERIFIED SOURCE | none | status=no_candidates ---\n'}]
        traces = [({'top': 'aaaaaaaaaaaa00'}, [('aaaaaaaaaaaa', 0.9), ('bbbbbbbbbbbb', 0.1)]),
                  ({'top': 'aaaaaaaaaaaa00'}, [('aaaaaaaaaaaa', 0.8)])]
        pools = qp.case_pools(fixture, messages, traces)
        self.assertEqual(sorted(pools), ['x', 'y'])
        self.assertEqual(len(pools['x']['scores']), 2)
        self.assertEqual(len(pools['y']['scores']), 1)


if __name__ == '__main__':
    unittest.main()
