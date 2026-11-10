"""Deployment-field isolation controls; no models/services."""
import copy
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
with patch.object(sys, 'path', [str(ROOT / 'tools'), *sys.path]):
    spec = importlib.util.spec_from_file_location('contract', ROOT / 'tools/answer_plumbing_contract.py')
    contract = importlib.util.module_from_spec(spec); spec.loader.exec_module(contract)
    from answer_bedrock_probe import UnixHTTP


class ContractTests(unittest.TestCase):
    def setUp(self):
        self.payload = {'messages': [{'role': 'user', 'content': 'Exact sources and current question'}],
                        'temperature': .7, 'top_k': 40, 'repeat_penalty': 1.05,
                        'top_p': .95, 'min_p': .05, 'seed': 42, 'max_tokens': 2048,
                        'stream': True, 'cache_prompt': True,
                        'chat_template_kwargs': {'enable_thinking': False}}

    def test_original_messages_and_native_sampling_are_preserved(self):
        original = copy.deepcopy(self.payload)
        arms = dict(contract.variants(self.payload, 2))
        for arm in arms.values():
            self.assertEqual(arm['messages'], self.payload['messages'])
            self.assertEqual(arm['seed'], 42)
            self.assertEqual(arm['max_tokens'], 2048)
            self.assertEqual(arm['min_p'], .05)
            self.assertEqual(arm['top_p'], .95)
        self.assertEqual(self.payload, original)

    def test_sampling_only_changes_three_core_fields(self):
        arms = dict(contract.variants(self.payload, 2))
        changed = {k for k in arms['q2_native'] if arms['q2_native'][k] != arms['q2_vendor_sampling'][k]}
        self.assertEqual(changed, {'temperature', 'top_k', 'repeat_penalty'})
        self.assertEqual(arms['q2_vendor_sampling']['temperature'], .1)
        self.assertEqual(arms['q2_vendor_sampling']['top_k'], 50)
        self.assertEqual(arms['q2_vendor_sampling']['repeat_penalty'], 1.1)

    def test_thinking_only_changes_the_flag(self):
        arms = dict(contract.variants(self.payload, 2))
        changed = {k for k in arms['q2_native'] if arms['q2_native'][k] != arms['q2_thinking'][k]}
        self.assertEqual(changed, {'chat_template_kwargs'})
        self.assertTrue(arms['q2_thinking']['chat_template_kwargs']['enable_thinking'])

    def test_unknown_native_configuration_refused(self):
        self.payload['chat_template_kwargs'] = {}
        with self.assertRaises(ValueError):
            contract.variants(self.payload, 2)

    def test_budget_pair_changes_only_reply_allowance(self):
        arms = dict(contract.budget_variants(self.payload))
        for condition in ('native', 'vendor_both'):
            low, high = arms[f'q6_{condition}_cap2048'], arms[f'q6_{condition}_cap8192']
            self.assertEqual({k for k in low if low[k] != high[k]}, {'max_tokens'})
            self.assertEqual(low['max_tokens'], 2048)
            self.assertEqual(high['max_tokens'], 8192)
            self.assertEqual(low['messages'], self.payload['messages'])

    def test_long_budget_has_a_transport_allowance_not_an_early_timeout(self):
        normal = UnixHTTP('unused-test-socket')
        extended = UnixHTTP('unused-test-socket', timeout=600)
        self.assertEqual(normal.timeout, 180)
        self.assertEqual(extended.timeout, 600)
        normal.close(); extended.close()

    def test_thinking_budget_changes_only_the_cap_at_each_question(self):
        for index in (2, 6):
            arms = dict(contract.budget_variants(self.payload, index, ('thinking',)))
            low, high = arms[f'q{index}_thinking_cap2048'], arms[f'q{index}_thinking_cap8192']
            self.assertEqual({k for k in low if low[k] != high[k]}, {'max_tokens'})
            self.assertTrue(low['chat_template_kwargs']['enable_thinking'])
            self.assertEqual(low['temperature'], .7)
            self.assertEqual(high['repeat_penalty'], 1.05)

    def test_remainder_uses_actual_count_and_retains_physical_reserve(self):
        with patch.object(contract, 'prompt_token_ids', return_value=[0]*5529):
            arms = dict(contract.remainder_variants(self.payload, None))
        low, high = arms['q6_native_cap2048'], arms['q6_native_remainder']
        self.assertEqual(high['max_tokens'], 2407)
        self.assertEqual({k for k in low if low[k] != high[k]}, {'max_tokens'})
        self.assertEqual(5529+high['max_tokens']+256, 8192)


if __name__ == '__main__':
    unittest.main()
