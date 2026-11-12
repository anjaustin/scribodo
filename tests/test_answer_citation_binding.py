"""Quote-to-source binding checker: pairing, normalization and classification; no models."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_citation_binding', ROOT / 'tools/answer_citation_binding.py')
cb = importlib.util.module_from_spec(spec); spec.loader.exec_module(cb)

PREFIX = 'document-name (untrusted data)="part01.md" | document=d | revision=r | SHA256=' + 'a'*64


def packet(*texts):
    blocks = '\n'.join(f'--- SOURCE {i} | {PREFIX} | unit={"%064x" % i} | bytes=0+{len(t)} ---\n{t}\n--- END SOURCE ---' for i, t in enumerate(texts, 1))
    return 'Retrieved.\n\n' + blocks + '\n' + cb.BOUNDARY + 'preview text about the weather in Hertfordshire\nQuestion?'


class CitationBindingTests(unittest.TestCase):
    def run_case(self, reply, *sources):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 'arm-request.json').write_text(json.dumps({'messages': [{'role': 'user', 'content': packet(*sources)}]}))
            (d / 'arm-response.json').write_text(json.dumps({'choices': [{'message': {'content': reply}}]}))
            return cb.analyze(str(d), 12)[0]

    def test_bound_misbound_unsupported_and_preview_are_distinguished(self):
        s1 = 'Mrs. Long says that Netherfield is taken by a young man of large fortune.'
        s2 = 'He had a fortune of twenty thousand pounds and spent it freely.'
        reply = ('From Source 1: “Netherfield is taken by a young man of large fortune”. '
                 'Source 1 also says “a fortune of twenty thousand pounds and spent it”. '
                 'And “the weather in Hertfordshire” plus “something the model simply made up here”.')
        r = self.run_case(reply, s1, s2)
        self.assertEqual((r['pieces'], r['bound_to_cited_source'], r['in_other_source_than_cited'],
                          r['only_in_attachment_preview'], r['in_no_supplied_text']), (4, 1, 1, 1, 1))

    def test_typographic_variants_and_ellipses_do_not_count_as_unsupported(self):
        s1 = 'It is a truth universally acknowledged, that a single man—in possession of a good fortune—must be in want of a wife.'
        reply = 'Source 1: "It is a truth universally acknowledged, that a single man - in possession… must be in want of a wife".'
        r = self.run_case(reply, s1)
        self.assertEqual((r['pieces'], r['bound_to_cited_source'], r['in_no_supplied_text']), (2, 2, 0))

    def test_a_marker_inside_a_word_is_part_of_the_word(self):
        # The C normalization's rule (emphasis_mark_at): markers fold at a word's edge and stay between letters.
        self.assertEqual(cb.normalize('har_dly *six* _years_ a*b'), 'har_dly six years a*b')
        s1 = 'she was hardly six years old, but she could ride any horse'
        self.assertEqual(self.run_case('Source 1: "she was har_dly six years old, but she could ride any horse".', s1)['in_no_supplied_text'], 1)
        self.assertEqual(self.run_case('Source 1: "she was _hardly_ six years old, but she could ride any horse".', s1)['bound_to_cited_source'], 1)

    def test_straight_and_curly_quotes_pair_independently(self):
        s1 = 'first supported sentence here'; s2 = 'second supported sentence here'
        reply = 'Source 1 gives “first supported sentence here” and Source 2 gives "second supported sentence here".'
        r = self.run_case(reply, s1, s2)
        self.assertEqual((r['pieces'], r['bound_to_cited_source'], r['in_other_source_than_cited']), (2, 2, 0))

    def test_label_case_does_not_matter(self):
        s1 = 'the first supported sentence of text'; s2 = 'the second supported sentence of text'
        reply = 'From **SOURCE 1**: "the first supported sentence of text". And source 2: "the second supported sentence of text".'
        r = self.run_case(reply, s1, s2)
        self.assertEqual((r['pieces'], r['bound_to_cited_source'], r['in_other_source_than_cited']), (2, 2, 0))

    def test_classify_matches_analyze(self):
        s1 = 'a supported sentence long enough to count'
        reply = 'Source 1 says "a supported sentence long enough to count" and "nothing in the packet says this one".'
        counts, details = cb.classify(packet(s1), reply, 12)
        r = self.run_case(reply, s1)
        self.assertEqual(counts, {k: r[k] for k in counts})
        self.assertEqual(len(details), 1)

    def test_reasoning_before_think_close_is_ignored(self):
        r = self.run_case('<think>“unsupported scratch quotation text”</think>\nNo quotes in the visible reply.', 'a source sentence')
        self.assertEqual(r['pieces'], 0)


if __name__ == '__main__':
    unittest.main()
