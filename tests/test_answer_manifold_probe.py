"""Pre-inference controls for the LMM experiment, no models/services."""
import copy
import importlib.util
from pathlib import Path
import re
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
with patch.object(sys, 'path', [str(ROOT / 'tools'), *sys.path]):
    spec = importlib.util.spec_from_file_location('manifold', ROOT / 'tools/answer_manifold_probe.py')
    manifold = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manifold)
    spec = importlib.util.spec_from_file_location('counterfact', ROOT / 'tools/answer_manifold_counterfact.py')
    counterfact = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(counterfact)


class CharTokenizer:
    def encode(self, text, **kwargs):
        return [ord(c) for c in text]

    def decode(self, ids, **kwargs):
        return ''.join(chr(c) for c in ids)

    def apply_chat_template(self, messages, **kwargs):
        return ''.join(m['role']+':'+m['content']+'\n' for m in messages)


class ManifoldTests(unittest.TestCase):
    def setUp(self):
        self.tok = CharTokenizer()
        self.unit = {'text': 'She gave him a living.'}
        self.question = 'What was given?'
        self.bound = '--- SOURCE 1 | unit=abc | bytes=0+21 ---\n'+self.unit['text']+'\n--- END SOURCE ---'
        self.payload = {'messages': [{'role': 'system', 'content': 'Native system'},
                                     {'role': 'user', 'content': 'Prior question?'},
                                     {'role': 'assistant', 'content': 'Prior erroneous response.'},
                                     {'role': 'user', 'content': self.bound + "\nAuthor's current request (not part of the retrieved sources):\nPreviews\n" + self.question}],
                        'temperature': .7, 'seed': 42, 'repeat_penalty': 1.05}

    def build(self):
        return manifold.build_arms(self.payload, self.question, self.unit, self.tok)

    def test_predeclared_arms_and_penalty_only_pair(self):
        arms, counts = self.build(); arms = dict(arms)
        self.assertEqual(list(arms), ['baseline', 'penalty_1', 'neutral_weather', 'neutral_room',
                                      'one_sentence', 'quote_only', 'oracle_one_sentence'])
        baseline = copy.deepcopy(arms['baseline']); baseline['repeat_penalty'] = 1.0
        self.assertEqual(baseline, arms['penalty_1'])
        self.assertEqual(counts['baseline'], counts['penalty_1'])

    def test_neutral_history_preserves_roles_counts_and_current_evidence(self):
        arms, counts = self.build(); arms = dict(arms)
        base = arms['baseline']
        for name in ('neutral_weather', 'neutral_room'):
            self.assertEqual(counts[name], counts['baseline'])
            self.assertEqual(arms[name]['messages'][0], base['messages'][0])
            self.assertEqual(arms[name]['messages'][-1], base['messages'][-1])
            for old, new in zip(base['messages'][1:-1], arms[name]['messages'][1:-1]):
                self.assertEqual(old['role'], new['role'])
                self.assertEqual(len(old['content']), len(new['content']))
                self.assertNotEqual(old['content'], new['content'])

    def test_constraints_only_append_and_original_is_unchanged(self):
        original = copy.deepcopy(self.payload)
        arms, _ = self.build(); arms = dict(arms)
        for name, suffix in (('one_sentence', manifold.SENTENCE), ('quote_only', manifold.QUOTE)):
            expected = copy.deepcopy(arms['baseline']); expected['messages'][-1]['content'] += suffix
            self.assertEqual(expected, arms[name])
        self.assertEqual(original, self.payload)
        self.assertIn(self.bound, arms['oracle_one_sentence']['messages'][-1]['content'])

    def test_counterfactual_is_synthetic_and_preserves_original(self):
        original = copy.deepcopy(self.payload)
        text = '“Lady Catherine de Bourgh,” she replied, “has very lately given him a living.”'
        arms = dict(counterfact.build_arms(self.payload, text, self.question))
        self.assertEqual(list(arms), ['living_sentence', 'living_quote', 'telescope_sentence', 'telescope_quote'])
        for name, arm in arms.items():
            content = arm['messages'][-1]['content']
            self.assertIn('Synthetic diagnostic reference', content)
            self.assertIn('synthetic-unit=', content)
            self.assertNotIn('revision=', content)
            self.assertIn('a brass telescope' if name.startswith('telescope') else 'a living', content)
        self.assertEqual(self.payload, original)

    def test_counterfactual_ambiguous_source_refused(self):
        with self.assertRaises(ValueError):
            counterfact.build_arms(self.payload, 'No target phrase', self.question)

    def test_neutral_roundtrip_mismatch_refused(self):
        class BadTokenizer(CharTokenizer):
            def decode(self, ids, **kwargs):
                return super().decode(ids, **kwargs)+' extra'
        with self.assertRaises(ValueError):
            manifold.neutral_text(BadTokenizer(), 'Original message', 'weather')

    def test_full_reply_allowance_is_protected(self):
        self.payload['messages'][2]['content'] = 'x'*7000
        with self.assertRaises(ValueError):
            self.build()

    def test_transfer_is_fixed_suffix_and_preserves_cross_question_context(self):
        class WordTokenizer(CharTokenizer):
            def encode(self, text, **kwargs):
                return list(range(len(re.findall(r'\w+|[^\w\s]', text))))
        payloads = {i: copy.deepcopy(self.payload) for i in range(7)}
        original = copy.deepcopy(payloads)
        arms, counts = manifold.build_transfer_arms(payloads, WordTokenizer())
        self.assertEqual(len(arms), 9)
        self.assertEqual(counts['neutral_suffix_question'], counts['one_sentence_replicate'])
        self.assertEqual(counts['neutral_suffix_end'], counts['one_sentence_replicate'])
        for name, arm in arms[3:]:
            self.assertTrue(name.startswith('question_'))
            self.assertEqual(arm['messages'][:-1], self.payload['messages'][:-1])
            self.assertEqual(arm['messages'][-1]['content'], self.payload['messages'][-1]['content']+manifold.SENTENCE)
        self.assertEqual(payloads, original)


if __name__ == '__main__':
    unittest.main()
