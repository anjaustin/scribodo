"""Gate replay driver: the claim extraction from a review sheet; no model, no binary."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_gate_replay', ROOT / 'tools/answer_gate_replay.py')
gr = importlib.util.module_from_spec(spec); spec.loader.exec_module(gr)


class GateReplayTests(unittest.TestCase):
    def test_items_carry_the_claim_both_spans_and_the_author_turns(self):
        sheet = {'cases': [
            {'id': 'contradictory_drafts', 'question': 'Which figure?', 'span': 'only three miles.',
             'second': {'span': 'He inquired how far Netherfield was.'}, 'gate': {'verdict': 'pass', 'answer': 'three miles; five miles'}},
            {'id': 'creative_request', 'question': 'Draft two sentences.', 'span': 'x', 'second': {}, 'gate': None},
            {'id': 'unavailable_fact', 'question': 'What is his name?', 'span': 'What is his name?', 'second': None,
             'gate': {'verdict': 'echoes_question', 'answer': 'Bennet'}},
        ]}
        fixture = {'contradictory_drafts': {'history': [{'role': 'user', 'content': 'Draft note: five miles.'}]}}
        items = gr.items_from_sheet(sheet, fixture, 'run1')
        self.assertEqual(len(items), 2)  # the ungated creative case is skipped
        item, record = items[0]
        self.assertEqual(item, {'answer': 'three miles; five miles', 'question': 'Which figure?',
                                'spans': ['only three miles.', 'He inquired how far Netherfield was.'], 'turns': ['Draft note: five miles.']})
        self.assertEqual(record, {'run': 'run1', 'case': 'contradictory_drafts', 'recorded': 'pass', 'answer': 'three miles; five miles'})
        item, record = items[1]
        self.assertEqual((item['spans'], item['turns'], record['recorded']), (['What is his name?'], [], 'echoes_question'))


if __name__ == '__main__':
    unittest.main()
