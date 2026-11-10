"""Clean-window review arm: arm construction, oracle and scoring; no model."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_clean_review', ROOT / 'tools/answer_clean_review.py')
cr = importlib.util.module_from_spec(spec); spec.loader.exec_module(cr)

UNIT_A = 'a' * 64
UNIT_B = 'b' * 64
TEXT_A = 'The eldest of them, a sensible, intelligent young woman, about twenty-seven, was Elizabeth’s intimate friend.'
TEXT_B = 'It was a journey of only twenty-four miles, and they began it so early as to be in Gracechurch Street by noon.'


def fixture():
    return {'review_fields': [], 'cases': [
        {'id': 'single_fact_charlotte_age', 'kind': 'single_fact', 'question': 'How old is Charlotte?',
         'designated': [{'document': 'part01.md', 'passage': 'about twenty-seven'}], 'expected': ''},
        {'id': 'unavailable_fact', 'kind': 'unavailable_fact', 'question': 'First name?', 'designated': [], 'expected': ''}]}


def units():
    return {UNIT_A: {'unit_id': UNIT_A, 'document_name': 'part01.md', 'text': TEXT_A, 'text_hash': 'h', 'revision_id': 'r', 'byte_start': 0},
            UNIT_B: {'unit_id': UNIT_B, 'document_name': 'part04.md', 'text': TEXT_B, 'text_hash': 'h', 'revision_id': 'r', 'byte_start': 0}}


def packet(*ids):
    return 'Retrieved\n\n' + ''.join(f'--- SOURCE {i+1} | document-name="x" | unit={u} | bytes=0+1 ---\n{units()[u]["text"]}\n--- END SOURCE ---\n'
                                     for i, u in enumerate(ids)) + "\nAuthor's current request (not part of the retrieved sources):\nq"


class CleanReviewTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(); self.addCleanup(self._tmp.cleanup)
        self.capture = Path(self._tmp.name)
        for i, ids in enumerate(((UNIT_A,), (UNIT_A, UNIT_B))):
            (self.capture / f'request-{i}.json').write_text(json.dumps(
                {'messages': [{'role': 'system', 'content': 's'}, {'role': 'user', 'content': packet(*ids)}]}))
        self.review = {'cases': [
            {'id': 'single_fact_charlotte_age', 'capture_index': 0,
             'visible_reply': 'From Source 1: “a sensible, intelligent young woman, about *twenty-seven*” and "she was seen walking twelve miles in the rain" so twenty-seven.'},
            {'id': 'unavailable_fact', 'capture_index': 1, 'visible_reply': 'His first name is Bennet.'}]}

    def test_arms_cover_claim_repeat_span_and_same_window(self):
        arms, oracle = cr.build_arms(fixture(), self.review, units(), self.capture)
        names = [a for a, _ in arms]
        self.assertEqual(names.count('claim:single_fact_charlotte_age:0'), 1)
        self.assertIn('claim2:single_fact_charlotte_age:0', names)
        self.assertIn('same:single_fact_charlotte_age', names)
        # No designated unit: every delivered unit is reviewed separately.
        self.assertIn('claim:unavailable_fact:0', names); self.assertIn('claim:unavailable_fact:1', names)
        spans = [n for n in names if n.startswith('span:')]
        self.assertEqual(len(spans), 2)
        self.assertTrue(oracle['span:single_fact_charlotte_age:0:0']['verbatim'])   # emphasis and curly quotes folded
        self.assertFalse(oracle['span:single_fact_charlotte_age:0:1']['verbatim'])  # invented quotation

    def test_requests_are_cold_deterministic_and_schema_bound(self):
        arms, _ = cr.build_arms(fixture(), self.review, units(), self.capture)
        for name, req in arms:
            self.assertEqual((req['temperature'], req['seed'], req['stream'], req['cache_prompt']), (0, 42, False, False))
            self.assertFalse(req['chat_template_kwargs']['enable_thinking'])
            self.assertTrue(req['response_format']['json_schema']['strict'])
            if name.startswith('same:'):
                self.assertEqual([m['role'] for m in req['messages']][-3:], ['user', 'assistant', 'user'])
            else:
                self.assertEqual([m['role'] for m in req['messages']], ['system', 'user'])
                self.assertNotIn('BEGIN DOCUMENT', req['messages'][-1]['content'].split('REPLY UNDER REVIEW')[0])

    def test_delivered_unit_must_match_exported_text(self):
        u = units(); u[UNIT_B]['text'] = 'edited'
        with self.assertRaises(ValueError):
            cr.build_arms(fixture(), self.review, u, self.capture)

    def test_scoring_against_predeclared_verdicts_and_byte_oracle(self):
        arms, oracle = cr.build_arms(fixture(), self.review, units(), self.capture)
        def row(name, content, usage=None):
            return {'arm': name, 'finish_reason': 'stop', 'usage': usage or {'prompt_tokens': 10},
                    'message': {'content': json.dumps(content)}}
        claim = {'reply_claim': 'twenty-seven', 'source_states': 'answers', 'reply_verdict': 'supported', 'quotations_verbatim': 'some'}
        wrong = {**claim, 'reply_verdict': 'contradicted'}
        results = [row('claim:single_fact_charlotte_age:0', claim), row('claim2:single_fact_charlotte_age:0', claim),
                   row('claim:unavailable_fact:0', wrong), row('claim2:unavailable_fact:0', claim),
                   row('span:single_fact_charlotte_age:0:0', {'verbatim_in_passage': True}),
                   row('span:single_fact_charlotte_age:0:1', {'verbatim_in_passage': True}),
                   row('same:single_fact_charlotte_age', claim),
                   {'arm': 'same:unavailable_fact', 'finish_reason': 'length', 'usage': {}, 'message': {'content': '{"reply_'}}]
        s = cr.score(results, oracle)
        self.assertEqual(s['summary']['claim_accepted'], 1)
        self.assertEqual(s['summary']['claim_total'], 2)
        self.assertEqual((s['claim_repeat_identical'], s['claim_repeat_differs']), (1, 1))
        self.assertEqual((s['summary']['span_correct'], s['summary']['span_false_positive']), (1, 1))
        self.assertEqual(s['summary']['same_accepted'], 1)
        self.assertEqual(s['unparsed'], 1)

    def test_constant_baselines_are_reported_beside_the_reviewer(self):
        # REDTEAM-020 R3: a reviewer that only matches the best constant answer has shown nothing.
        _, oracle = cr.build_arms(fixture(), self.review, units(), self.capture)
        s = cr.score([], oracle)
        self.assertEqual(s['constant_baselines'], {'supported': 1, 'contradicted': 0, 'unsupported': 2})
        self.assertEqual(s['summary']['best_constant_claim_accepted'], 2)


if __name__ == '__main__':
    unittest.main()
