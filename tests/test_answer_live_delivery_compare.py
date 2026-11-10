"""Live delivery comparison sheet: cross-run matching by content hash; no models or services."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_live_delivery_compare', ROOT / 'tools/answer_live_delivery_compare.py')
cmp_ = importlib.util.module_from_spec(spec); spec.loader.exec_module(cmp_)

CASE = {'id': 'single_fact_walk_distance', 'kind': 'single_fact', 'question': 'How far?',
        'designated': [{'document': 'part01.md', 'passage': 'only three miles'}]}


def make_run(root, unit_id, admitted, semantic_rank, in_request, reply):
    product = root / 'product'; capture = root / 'capture'; product.mkdir(); capture.mkdir()
    (product / 'units.json').write_text(json.dumps([
        {'unit_id': unit_id, 'text_hash': 'hash-three-miles', 'document_name': 'part01.md', 'text': 'It is only three miles.'},
        {'unit_id': 'other', 'text_hash': 'hash-other', 'document_name': 'part04.md', 'text': 'twenty-four miles'}]))
    (product / 'messages.json').write_text(json.dumps([
        {'seq': 1, 'role': 'user', 'content': 'How far?'},
        {'seq': 2, 'role': 'assistant', 'content': reply, 'memory_sources': unit_id if admitted else 'other', 'truncated': 0}]))
    semantic = [{'unit_id': 'other', 'text_hash': 'hash-other', 'score': .9}]
    if semantic_rank == 2:
        semantic.append({'unit_id': unit_id, 'text_hash': 'hash-three-miles', 'score': .5})
    (capture / 'retrieval-0.json').write_text(json.dumps({'query': 'How far?', 'semantic_ok': True, 'lexical_ok': True,
                                                          'semantic': semantic, 'lexical': []}))
    (capture / 'request-0.json').write_text(json.dumps({'messages': [{'role': 'user', 'content': 'only three miles' if in_request else 'x'}]}))
    return cmp_.load_run(product, capture)


class DeliveryCompareTests(unittest.TestCase):
    def test_units_match_across_runs_by_content_hash_and_fields_are_filled(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            old = make_run(Path(a), 'u-old-id', admitted=False, semantic_rank=None, in_request=False, reply='twenty-four')
            new = make_run(Path(b), 'u-new-id', admitted=True, semantic_rank=2, in_request=True, reply='three')
            out = cmp_.compare({'cases': [CASE]}, {'old': old, 'new': new})
        row = out['cases'][0]
        self.assertEqual((row['old']['designated'][0]['semantic_rank'], row['old']['designated'][0]['admitted']), (None, False))
        self.assertEqual((row['new']['designated'][0]['semantic_rank'], row['new']['designated'][0]['admitted'],
                          row['new']['designated'][0]['in_final_request']), (2, True, True))
        self.assertEqual(out['totals']['old']['admitted'], 0); self.assertEqual(out['totals']['new']['admitted'], 1)
        self.assertEqual(out['totals']['new']['semantic_candidate'], 1)
        self.assertEqual(out['totals']['replies_identical'], 0)

    def test_missing_question_or_ambiguous_binding_is_reported_not_scored(self):
        with tempfile.TemporaryDirectory() as a:
            run = make_run(Path(a), 'u', admitted=True, semantic_rank=2, in_request=True, reply='r')
            absent = cmp_.delivery(run, {**CASE, 'question': 'never asked'})
            self.assertIn('error', absent)
            unbound = cmp_.delivery(run, {**CASE, 'designated': [{'document': 'part01.md', 'passage': 'four miles'}]})
            self.assertIn('error', unbound['designated'][0])


if __name__ == '__main__':
    unittest.main()
