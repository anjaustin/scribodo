"""Coarse-to-fine reranking arm: deterministic parts only; no model, no server."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_window_rerank', ROOT / 'tools/answer_window_rerank.py')
wr = module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)


class WindowRerankTests(unittest.TestCase):
    def test_sentences_end_at_terminators_and_closing_quotes_but_not_honorifics(self):
        text = '“No, indeed, I did not.” “Her daughter will have a fortune.”  Mr. Collins spoke.\nShe smiled!'
        self.assertEqual(wr.sentences(text), ['“No, indeed, I did not.”', '“Her daughter will have a fortune.”',
                                              'Mr. Collins spoke.', 'She smiled!'])
        self.assertEqual(wr.sentences('   '), [])

    def test_windows_slide_over_sentences_and_a_short_unit_is_one_window(self):
        text = 'One. Two. Three. Four.'
        self.assertEqual(wr.windows(text, 3), [(0, 'One. Two. Three.'), (1, 'Two. Three. Four.')])
        self.assertEqual(wr.windows(text, 2, stride=2), [(0, 'One. Two.'), (2, 'Three. Four.')])
        self.assertEqual(wr.windows('One. Two.', 3), [(0, 'One. Two.')])
        self.assertEqual(wr.windows('', 3), [])

    def test_query_text_resolves_full_part_and_context_part_names(self):
        row = {'question': 'A, and B?', 'parts': ['A?', 'B?'], 'context_parts': ['C A?', 'C B?']}
        self.assertEqual(wr.query_text(row, 'full'), 'A, and B?')
        self.assertEqual(wr.query_text(row, 'part2'), 'B?')
        self.assertEqual(wr.query_text(row, 'cpart1'), 'C A?')


if __name__ == '__main__':
    unittest.main()
