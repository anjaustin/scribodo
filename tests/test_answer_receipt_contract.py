"""The receipt and trace contract from the reader's side (llm/answer_receipt.h).

tests/fixtures/answer_receipt/ is written by the C writers (test_answer_pipeline, test_receipt_contract, byte for
byte); this test parses the same files with the expressions tools/answer_pipeline_review.py uses on a run, and
applies the composer window's refusal rule (chat_receipt_refusal in C) in Python. A format change that breaks a
reader fails here; one that breaks the writer fails there. No models."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/fixtures/answer_receipt'
spec = importlib.util.spec_from_file_location('answer_pipeline_review', ROOT / 'tools/answer_pipeline_review.py')
pr = importlib.util.module_from_spec(spec); spec.loader.exec_module(pr)

TOP = 'f395a4d6679b2e5105afa2c52d388c820f756f40d13b71ef7256381837007e52'
SECOND = 'a20205a13f36e66794d13975faa7722e5f77f242866dfb6d5e035243c478aae8'


def refusal(receipt):
    """chat_receipt_refusal's rule: withheld=1, or the verdict no_answer."""
    g = pr.GATE.search(receipt)
    return bool(g) and (g[2] == '1' or g[1] == 'no_answer')


def lines(receipt):
    return receipt.split('\n')


class ReceiptContractTests(unittest.TestCase):
    def setUp(self):
        self.pass_ = (FIXTURES / 'receipt-pass.txt').read_text()
        self.withheld = (FIXTURES / 'receipt-withheld.txt').read_text()
        self.abstained = (FIXTURES / 'receipt-abstained.txt').read_text()
        self.trace = (FIXTURES / 'trace.log').read_text()

    def test_every_marker_line_is_terminated_and_fielded(self):
        for text in (self.pass_, self.withheld, self.abstained):
            self.assertTrue(text.endswith('\n'))
            for line in lines(text):
                if line.startswith('--- '):
                    self.assertTrue(line.endswith(' ---'), line)
                    fields = line[4:-4].split(' | ')
                    self.assertNotIn('', fields, line)
                    self.assertNotIn('\n', line)

    def test_pass_receipt_parses_two_sources_and_a_clear_gate(self):
        m = pr.RECEIPT.search(self.pass_)
        self.assertEqual((m[1], m[2].strip(), float(m[3]), m[4]), (TOP, 'part07.md', 0.9892, 'verified'))
        self.assertEqual(pr.PART.search(self.pass_.split('--- VERIFIED SOURCE 2')[0])[1], '1')
        first_span = self.pass_.split('---\n', 1)[1].split('\n', 1)[0]
        self.assertEqual(first_span, 'Quick as lightning they were upon him, and Tars Tarkas would have been gathered to his fathers.')
        s2 = pr.SECOND.search(self.pass_)
        self.assertEqual((s2[1], s2[2].strip(), float(s2[3]), s2[4], s2[5], s2[6].strip()),
                         (SECOND, 'part01.md', 0.9806, 'verified', '2', 'I cleared a good hundred and fifty feet;'))
        g = pr.GATE.search(self.pass_)
        self.assertEqual((g[1], g[2], g[3]), ('pass', '0', '150 feet'))
        q = pr.QUOTES.search(self.pass_)
        self.assertEqual((int(q[1]), q[2]), (1, ''))
        self.assertFalse(refusal(self.pass_))

    def test_withheld_receipt_keeps_the_whole_claim_line(self):
        m = pr.RECEIPT.search(self.withheld)
        self.assertEqual((m[1], m[4]), (TOP, 'verified'))
        self.assertIsNone(pr.SECOND.search(self.withheld))
        g = pr.GATE.search(self.withheld)
        # The claim had a newline (now a space) and contains " ---"; the field runs to the end of the line.
        self.assertEqual((g[1], g[2], g[3]), ('not_in_span', '1', 'sons of Tardos Mors --- or so'))
        q = pr.QUOTES.search(self.withheld)
        self.assertEqual((int(q[1]), q[2]), (2, '"the daughter of Mors Kajak"'))
        self.assertTrue(refusal(self.withheld))

    def test_abstained_receipt_has_the_none_header_and_is_a_refusal(self):
        m = pr.RECEIPT.search(self.abstained)
        self.assertIsNone(m[1]); self.assertEqual(m[5], 'no_candidates')
        self.assertEqual(lines(self.abstained)[0], '--- VERIFIED SOURCE | none | status=no_candidates | candidates=0 scored=0 ---')
        g = pr.GATE.search(self.abstained)
        self.assertEqual((g[1], g[2]), ('no_answer', '0'))
        self.assertTrue(refusal(self.abstained))

    def test_trace_lines_parse_in_order_with_scores_per_part(self):
        traces = pr.parse_trace(self.trace)
        self.assertEqual(len(traces), 3)
        t, scores = traces[0]
        self.assertEqual((t['candidates'], t['hydrated'], t['scored'], t['failures'], t['top'], t['score'], t['status']),
                         (52, 50, 6, 1, TOP, 0.9892, 'verified'))
        self.assertEqual((t['second'], t['second_score'], t['second_status'], t['parts'], t['top_part'], t['second_part']),
                         (SECOND, 0.9806, 'verified', 2, 1, 2))
        self.assertEqual((t['rerank_ms'], t['extract_ms'], t['request_bytes'], t['history_first']), (1234.6, 789.0, 4321, 3))
        self.assertEqual(scores, [('f395a4d6679b', 0.9892), ('a20205a13f36', 0.9806), ('c87be75a52ea', 0.1204)])
        self.assertEqual(t['part_scores'][2][0], ('f395a4d6679b', 0.9792))
        t, scores = traces[1]
        self.assertEqual((t['top'], t['second'], t['second_status'], t['parts'], t['second_part']), (TOP, '-', '-', 1, 0))
        self.assertEqual(len(scores), 3)
        t, scores = traces[2]
        self.assertEqual((t['status'], t['top'], t['parts'], t['request_bytes']), ('no_candidates', '-', 0, 3900))
        self.assertEqual(scores, [])
        gates = [l for l in self.trace.splitlines() if l.startswith('ANSWER_GATE ')]
        self.assertEqual([g.split()[1] for g in gates], ['verdict=pass', 'verdict=not_in_span', 'verdict=no_answer'])
        self.assertEqual(sum(1 for l in self.trace.splitlines() if l.startswith('ANSWER_INDEX_TEXT ')), 1)

    def test_recorded_run_receipts_still_parse_the_same(self):
        """The greedy field rule changes nothing on a recorded receipt without ' ---' inside a field."""
        recorded = ('--- ANSWER GATE | verdict=pass | withheld=0 | answer=thirty thousand souls; twenty thousand Warhoons ---\n'
                    '--- ANSWER QUOTES | checked=2 | misquoted= ---\n')
        g = pr.GATE.search(recorded)
        self.assertEqual(g[3], 'thirty thousand souls; twenty thousand Warhoons')
        self.assertEqual(pr.QUOTES.search(recorded)[2], '')


if __name__ == '__main__':
    unittest.main()
