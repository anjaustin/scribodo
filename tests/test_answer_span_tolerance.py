"""Span-verifier tolerance tool: the mutations change what they claim to change, and only there; no binary, no model."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_span_tolerance', ROOT / 'tools/answer_span_tolerance.py')
st = importlib.util.module_from_spec(spec); spec.loader.exec_module(st)

UNIT = ('“What is his name?”\n\n“Bingley.” Mr. Bennet’s property consisted almost entirely in an estate of two thousand '
        'a year. The hordes of Thark are divided into twenty-five communities, and the officers’ wives it’s said.')
# "Mr." counts as a sentence end for the tool as for the verifier's prefix rule, so the span starts after it.
SPAN = 'Bennet’s property consisted almost entirely in an estate of two thousand a year. The hordes of Thark are divided into twenty-five communities, and the officers’ wives it’s said.'


class ToleranceToolTests(unittest.TestCase):
    def test_meaning_mutations_land_inside_the_first_sentence(self):
        end = st.first_sentence_end(SPAN)
        self.assertTrue(SPAN[:end].endswith('a year.'))
        for name, fn in dict(st.FAMILIES['meaning']).items():
            mutated = fn(SPAN, UNIT)
            if mutated is None:
                continue
            with self.subTest(name):
                self.assertNotEqual(mutated, SPAN)
                # The tail after the first sentence is untouched, so the prefix rule cannot accept the mutation.
                self.assertTrue(mutated.endswith(SPAN[end:]), mutated)

    def test_each_mutation_does_what_its_name_says(self):
        self.assertIsNone(st.m_negate(SPAN, UNIT))  # no auxiliary and no negation in the first sentence
        self.assertEqual(st.first_sentence_end('Mr. Bennet slept. He woke.'), 3)
        self.assertIsNone(st.m_negate('He walked home.', UNIT))
        self.assertEqual(st.m_negate('He was tired. He slept.', UNIT), 'He was not tired. He slept.')
        self.assertEqual(st.m_negate("He wasn't tired. He slept.", UNIT), 'He was tired. He slept.')
        self.assertEqual(st.m_replace_name('Then Elizabeth said it was only three miles.', 'Jane and Elizabeth walked.'),
                         'Then Jane said it was only three miles.')
        self.assertIsNone(st.m_replace_name('Elizabeth said it was only three miles.', UNIT))  # a sentence-initial capital is not read as a name
        self.assertIsNone(st.m_replace_name('She said it was only three miles.', UNIT))  # pronouns are not names
        self.assertEqual(st.m_change_number('an estate of two thousand a year. More.', UNIT), 'an estate of three thousand a year. More.')
        self.assertEqual(st.m_lose_inner_apostrophe('it’s said. Yes.', UNIT), 'its said. Yes.')
        self.assertEqual(st.m_join_hyphen('twenty-five communities. Yes.', UNIT), 'twentyfive communities. Yes.')
        self.assertEqual(st.m_split_hyphen('twenty-five communities. Yes.', UNIT), 'twenty five communities. Yes.')
        self.assertEqual(st.m_lose_plural_possessive('the officers’ wives', UNIT), 'the officers wives')
        self.assertEqual(st.m_marker_inside_word('property consisted', UNIT), 'pro_perty consisted')
        self.assertEqual(st.m_no_break_spaces('a year. More.', UNIT), 'a\u00a0year.\u00a0More.')
        self.assertIsNone(st.m_no_break_spaces('Bingley.', UNIT))
        self.assertEqual(st.m_append_sentence('He slept.', UNIT), 'He slept. Nobody in the house knew of it.')
        self.assertIsNone(st.m_append_sentence('He slept', UNIT))
        self.assertEqual(st.m_negate_last_sentence('He slept. He was tired.', UNIT), 'He slept. He was not tired.')
        self.assertIsNone(st.m_negate_last_sentence('He was tired.', UNIT))

    def test_orthographic_mutations_change_no_word(self):
        words = lambda t: ''.join(ch for ch in t.lower() if ch.isalnum())
        for name, fn in st.FAMILIES['orthographic']:
            mutated = fn(SPAN, UNIT)
            if mutated is not None:
                with self.subTest(name):
                    self.assertEqual(words(mutated), words(SPAN))

    def test_pairs_come_from_the_verified_source_blocks_of_a_run(self):
        with tempfile.TemporaryDirectory() as d:
            run = Path(d) / 'run1'; run.mkdir()
            unit_id = 'a' * 64
            (run / 'units.json').write_text(json.dumps([{'unit_id': unit_id, 'text': UNIT}]))
            sources = (f'--- VERIFIED SOURCE | unit={unit_id} | document=x | score=0.5 | status=verified ---\n{SPAN}\n'
                       f'--- VERIFIED SOURCE 2 | unit={"b" * 64} | document=x | score=0.4 | status=verified ---\nunknown unit\n')
            (run / 'messages.json').write_text(json.dumps([{'memory_sources': sources}, {'memory_sources': ''}]))
            pairs = st.load_pairs(d)
        self.assertEqual(pairs, [('run1', 'verified', SPAN, UNIT)])


if __name__ == '__main__':
    unittest.main()
