"""Frozen-position and one-token arithmetic probe controls, no models."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
with patch.object(sys, 'path', [str(ROOT / 'tools'), *sys.path]):
    spec = importlib.util.spec_from_file_location('arithmetic', ROOT / 'tools/answer_plumbing_arithmetic.py')
    arithmetic = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(arithmetic)


class ArithmeticTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'; self.source.mkdir()
        self.run = self.root / 'out'
        self.case = {'index': 1, 'ids': [7, 8]}
        candidate = {'tokens': [3, 4, 5], 'completion_probabilities': [
            {'top_logprobs': [{'id': n, 'logprob': -.1}]} for n in (3, 4, 5)]}
        full = {'checks': [{'position': n, 'llama_max_top10_gap': g} for n, g in enumerate((.01, .6, .02))],
                'top': [[{'id': n, 'logprob': -.1}] for n in (3, 4, 5)]}
        for name, value in (('llama-1.json', {'response': candidate}), ('reference-full-1.json', full),
                            ('comparison.json', {'cases': [{'index': 1, 'shared_greedy_prefix': 2}]})):
            (self.source / name).write_text(json.dumps(value))

    def prepare(self):
        with patch.object(arithmetic, 'validate', return_value=[self.case]):
            arithmetic.prepare(SimpleNamespace(source=self.source, run=self.run))

    def test_exact_prefixes_and_preselected_positions(self):
        self.prepare()
        points = json.loads((self.run / 'points.json').read_text())
        self.assertEqual([p['position'] for p in points], [0, 1, 2])
        self.assertEqual([p['ids'] for p in points], [[7, 8], [7, 8, 3], [7, 8, 3, 4]])

    def test_one_token_cold_unpenalized_distribution(self):
        payload = arithmetic.completion([7, 8])
        self.assertEqual(payload['prompt'], [7, 8])
        self.assertEqual(payload['n_predict'], 1)
        self.assertEqual(payload['samplers'], ['top_k'])
        self.assertEqual(payload['repeat_penalty'], 1.0)
        self.assertFalse(payload['cache_prompt'])
        self.assertFalse(payload['post_sampling_probs'])

    def test_prefix_mutation_refused_before_model_access(self):
        self.prepare()
        with (self.run / 'points.json').open('a') as file:
            file.write(' ')
        with self.assertRaises(RuntimeError):
            arithmetic.frozen(SimpleNamespace(run=self.run))

    def test_backend_cache_factorial(self):
        self.assertEqual(set(arithmetic.CONFIGS.values()), {(0, 'bf16'), (99, 'bf16'), (0, 'f32'), (99, 'f32')})

    def test_cpu_disables_devices_and_operation_and_kv_offload(self):
        for config in ('cpu_bf16', 'cpu_f32'):
            flags = arithmetic.backend_flags(config)
            self.assertEqual(flags[flags.index('--device')+1], 'none')
            self.assertIn('--no-op-offload', flags)
            self.assertIn('--no-kv-offload', flags)
        self.assertNotIn('--device', arithmetic.backend_flags('metal_bf16'))


if __name__ == '__main__':
    unittest.main()
