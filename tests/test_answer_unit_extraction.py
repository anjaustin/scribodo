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
        self.assertEqual(ux.verify(None, unit), (None, 'unparsed'))  # REDTEAM-020 R2

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
        bind0 = dict.fromkeys(('pieces', 'bound_to_cited_source', 'in_other_source_than_cited', 'only_in_attachment_preview', 'in_no_supplied_text'), 0)
        record = [{'id': 'single_fact_charlotte_age', 'creative_composer': False, 'units': [
            {'expected_answers': True, 'answers': True, 'status': 'verified'},
            {'expected_answers': False, 'answers': True, 'status': 'rejected_not_in_unit'},
            {'expected_answers': False, 'answers': True, 'status': 'verified_prefix'},
            {'expected_answers': False, 'answers': False, 'status': 'none'}],
            'compose': {'compose': {'reply': 'r', 'checks': {'pass': True, 'markers': 0}, 'binding': bind0, 'contains_expected_true_span': True},
                        'compose_top1': {'reply': 'New writing: x', 'checks': {'pass': False, 'markers': 1}, 'binding': bind0, 'contains_expected_true_span': False},
                        'compose2': {'reply': 'r2'}, 'compose2_top1': {'reply': 'New writing: x'}},
            'ranking': {'realistic': [{'rank': 1, 'expected_answers': False, 'score': .9}, {'rank': 2, 'expected_answers': True, 'score': .8}]}}]
        s = ux.summarize(record)
        self.assertEqual((s['extract_calls'], s['answers_true'], s['verified'], s['verified_prefix'], s['rejected_not_in_unit']), (4, 3, 1, 1, 1))
        self.assertEqual((s['expected_true_verified'], s['expected_false_answered'], s['expected_false_verified']), (1, 2, 1))
        self.assertEqual(s['arms']['compose'], {'total': 1, 'pass': 1, 'with_expected_true_span': 1})
        self.assertEqual(s['arms']['compose_top1'], {'total': 1, 'pass': 0, 'with_expected_true_span': 0})
        self.assertEqual((s['compose_repeat_identical'], s['compose_repeat_differs']), (1, 1))
        self.assertEqual((s['markers_in_composed'], s['new_writing_on_factual']), (1, 1))
        self.assertEqual(s['ranks_of_expected_true']['realistic'], [{'case': 'single_fact_charlotte_age', 'rank': 2, 'of': 2, 'score': .8}])

    def test_uncapped_variant_and_sentence_prefix_verification(self):
        unit = units()[A]
        self.assertNotIn('maxLength', ux.SCHEMAS['span_uncapped']['properties']['span'])
        self.assertTrue(ux.answered({'span': 'x'}, 'span_uncapped')); self.assertFalse(ux.answered({'span': ''}, 'span_uncapped'))
        # A correct sentence followed by text that is not in the unit is cut back to the sentence.
        span = TEXT_A + ' Charlotte Lucas is twenty-seven years old.'
        got, status = ux.verify({'span': span}, unit, 'span_uncapped')
        self.assertEqual((status, got), ('verified_prefix', TEXT_A))
        # No sentence boundary inside the unit text: rejected, not partially accepted.
        self.assertEqual(ux.verify({'span': 'nonsense that is long enough. ' + TEXT_A}, unit, 'span_uncapped')[1], 'rejected_not_in_unit')
        self.assertIsNone(ux.sentence_prefix('short. no', unit['text']))

    def test_composer_split_and_ranking_math(self):
        factual = ux.compose_request('q', [('part01.md', 'about twenty-seven, was Elizabeth')])
        creative = ux.compose_request('q', [('part01.md', 'about twenty-seven, was Elizabeth')], creative=True)
        self.assertNotIn('new writing', factual['messages'][0]['content'])
        self.assertIn('new writing', creative['messages'][0]['content'])
        self.assertIn('do not guess', factual['messages'][0]['content'])
        q = [1.0, 0.0]; spans = [[0.0, 1.0], [0.6, 0.8], [1.0, 0.0]]
        self.assertEqual(ux.rank(q, spans), [(2, 1.0), (1, 0.6), (0, 0.0)])
        self.assertEqual(len(ux.clip_bytes('é' * 2000).encode()), 2000)  # cut on a character boundary
        self.assertTrue(ux.QUERY_INSTRUCTION.startswith('Instruct: ') and ux.QUERY_INSTRUCTION.endswith('Query: '))
    def test_checks_see_through_markdown_emphasis(self):
        # REDTEAM-020 R1: the fabrication screen must catch a bolded name.
        self.assertFalse(ux.check_reply('unavailable_fact', "Mr. Bennet's first name is **Bennet**.", added=False)['pass'])
        self.assertEqual(ux.check_reply('unavailable_fact', "Mr. Bennet's first name is **Bennet**.", added=False)['must_not'], [False])
        self.assertTrue(ux.check_reply('single_fact_charlotte_age', 'She is **twenty-seven**.', added=False)['pass'])

    def test_unparseable_extraction_is_counted_as_unparsed_not_none(self):
        # REDTEAM-020 R2: a reply cut by the token cap is not the model saying "none".
        unit = units()[A]
        self.assertEqual(ux.verify(None, unit, 'span_uncapped'), (None, 'unparsed'))
        self.assertEqual(ux.verify({'span': ''}, unit, 'span_uncapped'), (None, 'none'))
        record = [{'id': 'single_fact_charlotte_age', 'creative_composer': False, 'compose': {}, 'ranking': {},
                   'units': [{'expected_answers': False, 'answers': False, 'status': 'unparsed'}]}]
        s = ux.summarize(record)
        self.assertEqual((s['unparsed'], s['answers_true'], s['expected_false_answered']), (1, 0, 0))

    def test_plan_counts_match_the_plan(self):
        cases = ux.plan(fixture(), self.review, units(), self.capture)
        self.assertEqual(ux.plan_counts(cases), {'extract_calls': 4, 'extract_added_calls': 1, 'expected_true_units': 3,
                                                 'expected_true_realistic': 2, 'expected_true_added': 1, 'cases_with_added_units': 1})


if __name__ == '__main__':
    unittest.main()
