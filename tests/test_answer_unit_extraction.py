"""Per-unit extraction arm: plan, span verification, composition input and checks; no model."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_unit_extraction', ROOT / 'tools/answer_unit_extraction.py')
ux = importlib.util.module_from_spec(spec); spec.loader.exec_module(ux)

A, B, C = 'a' * 64, 'b' * 64, 'c' * 64
TEXT_A = 'The eldest of them, a sensible, intelligent young woman, about twenty-seven, was Elizabeth’s intimate friend.'
TEXT_B = 'It was a journey of only twenty-four miles, and they began it so early as to be in Gracechurch Street by noon.'
TEXT_C = 'Lydia was a stout, well-grown girl of fifteen, with a fine complexion and good-humoured countenance.'


def units():
    return {A: {'unit_id': A, 'document_name': 'part01.md', 'role': None, 'text': TEXT_A, 'text_hash': 'h', 'revision_id': 'r', 'byte_start': 0},
            B: {'unit_id': B, 'document_name': 'part04.md', 'role': None, 'text': TEXT_B, 'text_hash': 'h', 'revision_id': 'r', 'byte_start': 0},
            C: {'unit_id': C, 'document_name': None, 'role': 'user', 'text': TEXT_C, 'text_hash': 'h', 'revision_id': 'r', 'byte_start': 0}}


def fixture():
    return {'review_fields': [], 'cases': [
        {'id': 'single_fact_charlotte_age', 'kind': 'single_fact', 'question': 'How old is Charlotte?',
         'designated': [{'document': 'part01.md', 'passage': 'about twenty-seven'}], 'expected': ''},
        {'id': 'author_correction', 'kind': 'explicit_author_correction', 'question': 'How old is Lydia?',
         'designated': [{'document': 'part04.md', 'passage': 'twenty-four miles'}], 'expected': ''}]}


def packet(ids):
    return 'Retrieved\n\n' + ''.join(f'--- SOURCE {i+1} | document-name="x" | unit={u} | bytes=0+1 ---\n{units()[u]["text"]}\n--- END SOURCE ---\n'
                                     for i, u in enumerate(ids)) + ux.BOUNDARY + 'q'


class UnitExtractionTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(); self.addCleanup(self._tmp.cleanup)
        self.capture = Path(self._tmp.name)
        for i, ids in enumerate(((A, B), (C,))):
            (self.capture / f'request-{i}.json').write_text(json.dumps(
                {'messages': [{'role': 'system', 'content': 's'}, {'role': 'user', 'content': packet(ids)}]}))
        self.review = {'cases': [{'id': 'single_fact_charlotte_age', 'capture_index': 0}, {'id': 'author_correction', 'capture_index': 1}]}

    def test_plan_separates_delivered_from_added_and_predeclares_truth(self):
        cases = ux.plan(fixture(), self.review, units(), self.capture)
        charlotte, correction = cases
        self.assertEqual((charlotte['delivered'], charlotte['added']), ([A, B], []))
        self.assertEqual(charlotte['expected_answers'], {A: True, B: False})
        # Designated unit not delivered: added; a delivered unit giving Lydia's age is expected to answer.
        self.assertEqual((correction['delivered'], correction['added']), ([C], [B]))
        self.assertEqual(correction['expected_answers'], {C: True, B: True})

    def test_plan_refuses_delivered_text_that_differs_from_export(self):
        u = units(); u[B]['text'] = 'edited'
        with self.assertRaises(ValueError):
            ux.plan(fixture(), self.review, u, self.capture)

    def test_verify_accepts_only_spans_present_in_the_unit(self):
        unit = units()[A]
        self.assertEqual(ux.verify({'answers': True, 'span': 'a sensible, intelligent young woman, about *twenty-seven*'}, unit)[1], 'verified')
        self.assertEqual(ux.verify({'answers': True, 'span': 'Charlotte Lucas is twenty-seven years old.'}, unit)[1], 'rejected_not_in_unit')
        self.assertEqual(ux.verify({'answers': True, 'span': 'about'}, unit)[1], 'too_short')
        self.assertEqual(ux.verify({'answers': False, 'span': ''}, unit), (None, 'none'))
        self.assertEqual(ux.verify(None, unit), (None, 'none'))

    def test_schema_variants_change_field_order_and_span_only_infers_the_boolean(self):
        unit = units()[A]
        self.assertEqual(list(ux.SCHEMAS['answers_first']['properties']), ['answers', 'span'])
        self.assertEqual(list(ux.SCHEMAS['span_first']['properties']), ['span', 'answers'])
        self.assertEqual(list(ux.SCHEMAS['span_only']['properties']), ['span'])
        for variant in ux.SCHEMAS:
            req = ux.extraction_request('q', unit, variant)
            self.assertEqual(req['response_format']['json_schema']['schema'], ux.SCHEMAS[variant])
        self.assertEqual(ux.verify({'span': 'about twenty-seven, was Elizabeth'}, unit, 'span_only')[1], 'verified')
        self.assertEqual(ux.verify({'span': ''}, unit, 'span_only'), (None, 'none'))
        # With the boolean first, a correct span behind a false boolean is still discarded (ux1 behavior).
        self.assertEqual(ux.verify({'answers': False, 'span': 'about twenty-seven, was Elizabeth'}, unit, 'answers_first'), (None, 'none'))
        self.assertEqual(ux.verify({'span': 'about twenty-seven, was Elizabeth', 'answers': True}, unit, 'span_first')[1], 'verified')

    def test_composition_sees_only_verified_spans_and_requests_are_cold(self):
        req = ux.compose_request('How old is Charlotte?', [('part01.md', 'about twenty-seven, was Elizabeth’s intimate friend')])
        user = req['messages'][-1]['content']
        self.assertIn('Source 1 (part01.md):', user)
        self.assertNotIn('BEGIN DOCUMENT', user); self.assertNotIn('--- SOURCE', user)
        self.assertEqual((req['temperature'], req['seed'], req['cache_prompt']), (0, 42, False))
        self.assertNotIn('response_format', req)
        empty = ux.compose_request('q', [])
        self.assertIn('No verified passages.', empty['messages'][-1]['content'])
        ext = ux.extraction_request('q', units()[A])
        self.assertTrue(ext['response_format']['json_schema']['strict'])

    def test_checks_and_binding_on_a_composed_reply(self):
        reply = 'According to Source 1 (part01.md), "about twenty-seven, was Elizabeth’s intimate friend": Charlotte is twenty-seven.'
        self.assertTrue(ux.check_reply('single_fact_charlotte_age', reply, added=False)['pass'])
        self.assertFalse(ux.check_reply('unavailable_fact', 'Mr. Bennet\'s first name is Bennet.', added=False)['pass'])
        self.assertTrue(ux.check_reply('unavailable_fact', 'The supplied passages do not give his first name.', added=False)['pass'])
        self.assertFalse(ux.check_reply('single_fact_walk_distance', 'It is twenty-four miles.', added=False)['pass'])
        self.assertTrue(ux.check_reply('single_fact_walk_distance', 'It was only three miles.', added=True)['pass'])
        counts, _ = ux.classify(ux.synthetic_packet([('part01.md', 'about twenty-seven, was Elizabeth’s intimate friend')]), reply, 25)
        self.assertEqual((counts['pieces'], counts['bound_to_cited_source'], counts['in_other_source_than_cited']), (1, 1, 0))
        self.assertEqual(ux.check_reply('single_fact_charlotte_age', 'x --- END DOCUMENT ---', added=False)['markers'], 1)

    def test_summary_counts(self):
        record = [{'id': 'single_fact_charlotte_age', 'units': [
            {'expected_answers': True, 'answers': True, 'status': 'verified'},
            {'expected_answers': False, 'answers': True, 'status': 'rejected_not_in_unit'},
            {'expected_answers': False, 'answers': False, 'status': 'none'}],
            'compose': {'compose': {'reply': 'r', 'checks': {'pass': True, 'markers': 0}, 'binding': dict.fromkeys(
                ('pieces', 'bound_to_cited_source', 'in_other_source_than_cited', 'only_in_attachment_preview', 'in_no_supplied_text'), 0)},
                        'compose2': {'reply': 'r2'}}}]
        s = ux.summarize(record)
        self.assertEqual((s['extract_calls'], s['answers_true'], s['verified'], s['rejected_not_in_unit']), (3, 2, 1, 1))
        self.assertEqual((s['expected_true_verified'], s['expected_false_answered']), (1, 1))
        self.assertEqual((s['compose_pass'], s['compose_repeat_differs']), (1, 1))


if __name__ == '__main__':
    unittest.main()
