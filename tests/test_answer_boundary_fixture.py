"""Boundary-question fixture and binder invariants; no models or services."""
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/answer_boundary/questions.json'
# Each held-out fixture and the UI script generated from it (tools/answer_boundary_script.py).
FIXTURE_SCRIPTS = {
    FIXTURE: ROOT / 'tests/ui/store/21_boundary_questions.scribo',
    ROOT / 'tests/fixtures/answer_boundary/princess-of-mars.json': ROOT / 'tests/ui/store/22_mars_boundary_questions.scribo',
    ROOT / 'tests/fixtures/answer_boundary/wuthering-heights.json': ROOT / 'tests/ui/store/23_heights_boundary_questions.scribo',
}
spec = importlib.util.spec_from_file_location('answer_boundary_bind', ROOT / 'tools/answer_boundary_bind.py')
binder = importlib.util.module_from_spec(spec); spec.loader.exec_module(binder)
REQUIRED_KINDS = {'single_fact', 'two_entities_different_values', 'explicit_author_correction',
                  'contradictory_dated_drafts', 'unavailable_fact', 'creative_speculative'}
SEVEN_QUESTION_SCRIPT = ROOT / 'tests/ui/store/18_manuscript_questions.scribo'


class BoundaryFixtureTests(unittest.TestCase):
    def setUp(self):
        self.fixtures = {path: json.loads(path.read_text()) for path in FIXTURE_SCRIPTS}
        self.fixture = self.fixtures[FIXTURE]

    def test_every_required_kind_is_present_and_cases_are_complete(self):
        for path, fixture in self.fixtures.items():
            kinds = {c['kind'] for c in fixture['cases']}
            self.assertTrue(REQUIRED_KINDS <= kinds, (path.name, REQUIRED_KINDS - kinds))
            ids = [c['id'] for c in fixture['cases']]
            self.assertEqual(len(ids), len(set(ids)), path.name)
            for c in fixture['cases']:
                self.assertTrue(c['question'].strip() and c['expected'].strip(), (path.name, c['id']))
                self.assertEqual(bool(c['designated']) or c['kind'] == 'unavailable_fact', True, (path.name, c['id']))

    def test_questions_are_fresh(self):
        script = SEVEN_QUESTION_SCRIPT.read_text()
        questions = {}
        for path, fixture in self.fixtures.items():
            for c in fixture['cases']:
                self.assertNotIn(c['question'], script, (path.name, c['id']))
                # The second manuscript's questions must not reuse the first's.
                self.assertNotIn(c['question'], questions, (path.name, c['id']))
                questions[c['question']] = path.name

    def test_binder_refuses_ambiguous_and_missing_passages(self):
        units = [{'unit_id': 'u1', 'document_name': 'part01.md', 'text': 'alpha beta gamma', 'text_hash': 'h', 'revision_id': 'r', 'byte_start': 0},
                 {'unit_id': 'u2', 'document_name': 'part01.md', 'text': 'alpha beta delta', 'text_hash': 'h', 'revision_id': 'r', 'byte_start': 9}]
        ok = {'cases': [{'id': 'a', 'designated': [{'document': 'part01.md', 'passage': 'gamma'}]}]}
        self.assertEqual(binder.bind(ok, units)[0]['designated'][0]['unit_id'], 'u1')
        for passage in ('alpha beta', 'omega'):
            with self.assertRaises(ValueError):
                binder.bind({'cases': [{'id': 'b', 'designated': [{'document': 'part01.md', 'passage': passage}]}]}, units)

    def test_generated_script_matches_fixture(self):
        for path, script_path in FIXTURE_SCRIPTS.items():
            script = script_path.read_text()
            for c in self.fixtures[path]['cases']:
                self.assertIn(f'type "{c["question"]}"', script, (path.name, c['id']))
                for note in c.get('history', []):
                    self.assertIn(f'type "{note["content"]}"', script, (path.name, c['id']))
                for d in c['designated']:
                    self.assertIn(f'expect memory_sources "{d["passage"][:40]}"', script, (path.name, c['id']))


if __name__ == '__main__':
    unittest.main()
