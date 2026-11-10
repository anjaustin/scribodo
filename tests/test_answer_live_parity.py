"""Synthetic numerical comparisons; never load a tokenizer or model."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
with patch.object(sys, 'path', [str(ROOT / 'tools'), *sys.path]):
    spec = importlib.util.spec_from_file_location('live_parity', ROOT / 'tools/answer_live_parity.py')
    parity = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(parity)


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.run = Path(self.temp.name)
        self.case = {'index': 1, 'question': 'synthetic', 'ids': [1, 2], 'native_reply': {}}
        self.tokens = [3, 4, 5]
        self.candidate = {'tokens': self.tokens, 'content': 'synthetic', 'completion_probabilities': [
            {'top_logprobs': [{'id': 3, 'logprob': -.1}, {'id': 4, 'logprob': -1}]},
            {'top_logprobs': [{'id': 4, 'logprob': -.1}, {'id': 5, 'logprob': -1}]},
            {'top_logprobs': [{'id': 5, 'logprob': -.1}, {'id': 4, 'logprob': -1}]}]}
        self.reference = {'teacher_forced_candidate_tokens': self.tokens[:], 'top': [
            [{'id': 3, 'logprob': -.1}, {'id': 4, 'logprob': -1}],
            [{'id': 5, 'logprob': -.09}, {'id': 4, 'logprob': -.1}],
            [{'id': 5, 'logprob': -.1}, {'id': 4, 'logprob': -1}]]}

    def compare(self):
        (self.run / 'llama-1.json').write_text(json.dumps({'response': self.candidate}))
        (self.run / 'reference-1.json').write_text(json.dumps(self.reference))
        with patch.object(parity, 'validate', return_value=[self.case]), contextlib.redirect_stdout(io.StringIO()):
            parity.compare(SimpleNamespace(run=self.run))

    def test_shared_prefix_stops_at_first_divergence_and_is_not_quality_pass(self):
        self.compare()
        result = json.loads((self.run / 'comparison.json').read_text())
        self.assertFalse(result['quality_pass'])
        self.assertEqual(result['cases'][0]['shared_greedy_prefix'], 1)
        self.assertEqual(len(result['cases'][0]['positions']), 3)

    def test_different_forced_prefix_is_refused(self):
        self.reference['teacher_forced_candidate_tokens'][0] = 9
        with self.assertRaises(RuntimeError):
            self.compare()

    def test_missing_position_is_refused(self):
        self.reference['top'].pop()
        with self.assertRaises(RuntimeError):
            self.compare()

    def test_actual_slot_is_8k(self):
        self.assertEqual(parity.SLOT_TOKENS, 8192)


if __name__ == '__main__':
    unittest.main()
