"""Boundary-question fixture and binder invariants; no models or services."""
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/answer_boundary/questions.json'
spec = importlib.util.spec_from_file_location('answer_boundary_bind', ROOT / 'tools/answer_boundary_bind.py')
binder = importlib.util.module_from_spec(spec); spec.loader.exec_module(binder)
REQUIRED_KINDS = {'single_fact', 'two_entities_different_values', 'explicit_author_correction',
                  'contradictory_dated_drafts', 'unavailable_fact', 'creative_speculative'}
SEVEN_QUESTION_SCRIPT = ROOT / 'tests/ui/store/18_manuscript_questions.scribo'


class BoundaryFixtureTests(unittest.TestCase):
    def setUp(self):
        self.fixture = json.loads(FIXTURE.read_text())

    def test_every_required_kind_is_present_and_cases_are_complete(self):
        kinds = {c['kind'] for c in self.fixture['cases']}
        self.assertTrue(REQUIRED_KINDS <= kinds, REQUIRED_KINDS - kinds)
        ids = [c['id'] for c in self.fixture['cases']]
        self.assertEqual(len(ids), len(set(ids)))
        for c in self.fixture['cases']:
            self.assertTrue(c['question'].strip() and c['expected'].strip(), c['id'])
            self.assertEqual(bool(c['designated']) or c['kind'] == 'unavailable_fact', True, c['id'])

    def test_questions_are_fresh(self):
        script = SEVEN_QUESTION_SCRIPT.read_text()
        for c in self.fixture['cases']:
            self.assertNotIn(c['question'], script, c['id'])

    def test_binder_refuses_ambiguous_and_missing_passages(self):
        units = [{'unit_id': 'u1', 'document_name': 'part01.md', 'text': 'alpha beta gamma', 'text_hash': 'h', 'revision_id': 'r', 'byte_start': 0},
                 {'unit_id': 'u2', 'document_name': 'part01.md', 'text': 'alpha beta delta', 'text_hash': 'h', 'revision_id': 'r', 'byte_start': 9}]
        ok = {'cases': [{'id': 'a', 'designated': [{'document': 'part01.md', 'passage': 'gamma'}]}]}
        self.assertEqual(binder.bind(ok, units)[0]['designated'][0]['unit_id'], 'u1')
        for passage in ('alpha beta', 'omega'):
            with self.assertRaises(ValueError):
                binder.bind({'cases': [{'id': 'b', 'designated': [{'document': 'part01.md', 'passage': passage}]}]}, units)

    def test_generated_script_matches_fixture(self):
        script = (ROOT / 'tests/ui/store/21_boundary_questions.scribo').read_text()
        for c in self.fixture['cases']:
            self.assertIn(f'type "{c["question"]}"', script, c['id'])
            for note in c.get('history', []):
                self.assertIn(f'type "{note["content"]}"', script, c['id'])


if __name__ == '__main__':
    unittest.main()
