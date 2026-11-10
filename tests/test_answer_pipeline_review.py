"""Pipeline review sheet: trace parsing, receipt parsing, designated ranks; no models."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_pipeline_review', ROOT / 'tools/answer_pipeline_review.py')
pr = importlib.util.module_from_spec(spec); spec.loader.exec_module(pr)

LOG = """noise
ANSWER_PIPELINE candidates=3 hydrated=3 scored=3 failures=0 top=aaaaaaaaaaaa1111 score=0.9900 status=verified rerank_ms=12.5 extract_ms=3.0 request_bytes=100 history_first=4
ANSWER_PIPELINE_SCORES aaaaaaaaaaaa:0.9900 bbbbbbbbbbbb:0.5000 cccccccccccc:0.1000
ANSWER_PIPELINE candidates=0 hydrated=0 scored=0 failures=0 top=- score=0.0000 status=no_candidates rerank_ms=0.0 extract_ms=0.0 request_bytes=90
ANSWER_PIPELINE candidates=2 hydrated=2 scored=2 failures=0 top=aaaaaaaaaaaa1111 score=0.9000 status=verified second=bbbbbbbbbbbb2222 second_score=0.8000 second_status=none rerank_ms=1.0 extract_ms=2.0 request_bytes=80 history_first=1
ANSWER_PIPELINE_SCORES aaaaaaaaaaaa:0.9000 bbbbbbbbbbbb:0.8000
"""
UNITS = [{'unit_id': 'a' * 12 + '1111', 'document_name': 'part01.md', 'text': 'It was only three miles. More.'},
         {'unit_id': 'b' * 12 + '2222', 'document_name': 'part04.md', 'text': 'twenty-four miles'}]
CASE = {'id': 'single_fact_walk_distance', 'kind': 'single_fact', 'question': 'How far?', 'expected': 'three miles',
        'designated': [{'document': 'part01.md', 'passage': 'only three miles'}]}


class PipelineReviewTests(unittest.TestCase):
    def test_trace_parsing_pairs_lines_and_tolerates_missing_scores(self):
        traces = pr.parse_trace(LOG)
        self.assertEqual(len(traces), 3)
        self.assertEqual((traces[2][0]['second'], traces[2][0]['second_score'], traces[2][0]['second_status']), ('b' * 12 + '2222', .8, 'none'))
        self.assertIsNone(traces[0][0]['second'])
        self.assertEqual(traces[0][0]['scored'], 3); self.assertEqual(traces[0][0]['history_first'], 4)
        self.assertEqual(traces[0][1], [('a' * 12, .99), ('b' * 12, .5), ('c' * 12, .1)])
        self.assertEqual((traces[1][0]['status'], traces[1][1]), ('no_candidates', []))

    def test_review_marks_chosen_designated_rank_and_span(self):
        receipt = '--- VERIFIED SOURCE | unit=' + 'a' * 12 + '1111 | document=part01.md | score=0.9900 | status=verified | candidates=3 scored=3 ---\nIt was only three miles.\n'
        messages = [{'seq': 1, 'role': 'user', 'content': 'How far?'},
                    {'seq': 2, 'role': 'assistant', 'content': 'Three miles.', 'memory_sources': receipt, 'truncated': 0}]
        # A trace-less turn before the question must not shift the pairing: traces are matched by the chosen unit id.
        shifted = [({'top': 'ffffffffffff0000', 'scored': 9, 'history_first': 1}, [('f' * 12, .2)])] + pr.parse_trace(LOG)
        sheet = pr.review({'review_fields': ['completion'], 'cases': [CASE]}, messages, UNITS, shifted)
        row = sheet[0]
        self.assertEqual(row['chosen']['document'], 'part01.md')
        d = row['designated'][0]
        self.assertTrue(d['chosen']); self.assertEqual((d['candidate_rank'], d['candidate_score']), (1, .99)); self.assertTrue(d['passage_in_span'])
        self.assertEqual(row['span'], 'It was only three miles.')
        # A run with no receipt line and a designated unit outside the candidates reports None, not a guess.
        messages[1]['memory_sources'] = '--- VERIFIED SOURCE | none | status=no_candidates | candidates=0 scored=0 ---\n'
        other = pr.review({'review_fields': [], 'cases': [{**CASE, 'designated': [{'document': 'part04.md', 'passage': 'twenty-four miles'}]}]},
                          messages, UNITS, [({}, [])])
        self.assertEqual(other[0]['chosen'], {'unit_id': None, 'status': 'no_candidates'})
        self.assertIsNone(other[0]['designated'][0]['candidate_rank']); self.assertFalse(other[0]['designated'][0]['chosen'])

    def test_same_unit_chosen_twice_consumes_traces_in_reply_order(self):
        unit = 'a' * 12 + '1111'
        receipt = '--- VERIFIED SOURCE | unit=' + unit + ' | document=part01.md | score=0.9900 | status=verified | candidates=3 scored=3 ---\nIt was only three miles.\n'
        messages = [{'seq': 1, 'role': 'user', 'content': 'first?'}, {'seq': 2, 'role': 'assistant', 'content': 'x', 'memory_sources': receipt},
                    {'seq': 3, 'role': 'user', 'content': 'How far?'}, {'seq': 4, 'role': 'assistant', 'content': 'y', 'memory_sources': receipt}]
        traces = [({'top': unit, 'scored': 1, 'history_first': 0}, [('a' * 12, .5)]), ({'top': unit, 'scored': 2, 'history_first': 2}, [('a' * 12, .9)])]
        sheet = pr.review({'review_fields': [], 'cases': [CASE]}, messages, UNITS, traces)
        self.assertEqual(sheet[0]['trace']['scored'], 2)
        self.assertEqual(sheet[0]['designated'][0]['candidate_score'], .9)


if __name__ == '__main__':
    unittest.main()
