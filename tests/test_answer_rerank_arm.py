"""Cross-encoder and lexical ranking arm: deterministic parts only; no models, no torch."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_rerank_arm', ROOT / 'tools/answer_rerank_arm.py')
ra = importlib.util.module_from_spec(spec); spec.loader.exec_module(ra)

QUESTION = 'How far does Elizabeth say the walk to Netherfield is when she sets out to see Jane?'


class RerankArmTests(unittest.TestCase):
    def test_lexical_overlap_counts_distinct_content_words_case_insensitively(self):
        words = ra.content_words(QUESTION)
        self.assertEqual(words, ['far', 'elizabeth', 'say', 'walk', 'netherfield', 'sets', 'see', 'jane'])
        unit = 'A servant from NETHERFIELD brought the following note for Elizabeth: ... only three miles. I shall be back by dinner.'
        self.assertAlmostEqual(ra.lexical_score(QUESTION, unit), 2 / 8)
        self.assertEqual(ra.lexical_score(QUESTION, 'nothing in common'), 0.0)
        self.assertEqual(ra.lexical_score('the of and', 'anything'), 0.0)
        self.assertAlmostEqual(ra.lexical_score('Elizabeth Elizabeth walk', 'elizabeth walked'), 1 / 2)  # whole words, distinct

    def test_rerank_prompt_is_the_model_card_format(self):
        p = ra.rerank_prompt('q?', 'doc text')
        self.assertTrue(p.startswith('<|im_start|>system\nJudge whether the Document meets the requirements'))
        self.assertIn('<Instruct>: Given a web search query, retrieve relevant passages that answer the query\n<Query>: q?\n<Document>: doc text', p)
        self.assertTrue(p.endswith('<|im_start|>assistant\n<think>\n\n</think>\n\n'))

    def test_ranking_rows_and_pipeline_shape(self):
        items = [{'unit_id': 'u1', 'expected': False}, {'unit_id': 'u2', 'expected': True}, {'unit_id': 'u3', 'expected': False}]
        rows = ra.rows_from_scores([0.2, 0.9, 0.9], items)
        self.assertEqual([r['unit_id'] for r in rows], ['u2', 'u3', 'u1'])  # ties by index
        emb = [{'unit_id': 'u1', 'rank': 1}, {'unit_id': 'u2', 'rank': 2}, {'unit_id': 'u3', 'rank': 3}]
        by_unit = {it['unit_id']: it for it in items}
        piped = ra.pipeline_rows(emb, {'u1': 0.1, 'u2': 0.8, 'u3': 0.5}, by_unit, top_n=2)
        self.assertEqual([(r['unit_id'], r['rank'], r['score']) for r in piped], [('u2', 1, 0.8), ('u1', 2, 0.1), ('u3', 3, None)])
        self.assertEqual(ra.expected_ranks(piped)[0]['rank'], 1)

    def test_reranker_is_pinned(self):
        self.assertEqual(ra.RERANKER, 'Qwen/Qwen3-Reranker-0.6B')
        self.assertRegex(ra.RERANKER_REVISION, r'^[0-9a-f]{40}$')
        self.assertEqual(ra.TOP_N, 32)

    def test_documents_only_view_reranks_without_chat_units(self):
        rows = [{'rank': 1, 'score': .9, 'label': 'chat', 'unit_id': 'c1', 'kind': 'chat', 'expected': False},
                {'rank': 2, 'score': .8, 'label': 'part01.md', 'unit_id': 'd1', 'kind': 'document', 'expected': True},
                {'rank': 3, 'score': .7, 'label': 'part02.md', 'unit_id': 'd2', 'kind': 'document', 'expected': False}]
        view = ra.documents_only(rows)
        self.assertEqual(view['expected'][0]['rank'], 1); self.assertEqual(view['expected'][0]['of'], 2)
        self.assertEqual([r['unit_id'] for r in view['top3']], ['d1', 'd2'])


if __name__ == '__main__':
    unittest.main()
