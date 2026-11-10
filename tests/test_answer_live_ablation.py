"""No-model controls for live request component removals."""
import copy
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
with patch.object(sys, 'path', [str(ROOT / 'tools'), *sys.path]):
    spec = importlib.util.spec_from_file_location('ablation', ROOT / 'tools/answer_live_ablation.py')
    ablation = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ablation)


class AblationTests(unittest.TestCase):
    def setUp(self):
        self.question = 'What was given?'
        self.unit = {'text': 'She gave him a living.'}
        self.block = '--- SOURCE 2 | document=original | unit=abc | bytes=0+21 ---\nShe gave him a living.\n--- END SOURCE ---'
        self.reference = 'Original reference header\n'+self.block
        self.preview = 'Attached document previews\n'
        self.payload = {'messages': [{'role': 'system', 'content': 'Unchanged native instructions'},
                                     {'role': 'user', 'content': 'Older question'},
                                     {'role': 'assistant', 'content': 'Older answer'},
                                     {'role': 'user', 'content': self.reference + ablation.BOUNDARY + self.preview + self.question}],
                        'seed': 42, 'stream': True, 'temperature': .7, 'repeat_penalty': 1.05,
                        'chat_template_kwargs': {'enable_thinking': False}}

    def arms(self):
        return dict(ablation.variants(self.payload, self.question, self.unit))

    def test_original_unchanged_and_other_sampling_preserved(self):
        original = copy.deepcopy(self.payload)
        for arm in self.arms().values():
            self.assertEqual(arm['messages'][0], original['messages'][0])
            self.assertEqual(arm['repeat_penalty'], 1.05)
            self.assertEqual(arm['seed'], 42)
            self.assertEqual(arm['max_tokens'], 2048)
            self.assertFalse(arm['stream'])
            self.assertFalse(arm['cache_prompt'])
            self.assertEqual(arm['temperature'], 0)
        self.assertEqual(self.payload, original)

    def test_single_component_removals(self):
        arms = self.arms()
        self.assertEqual(arms['full']['messages'], self.payload['messages'])
        self.assertEqual(arms['no_history']['messages'][-1], self.payload['messages'][-1])
        self.assertEqual(len(arms['no_history']['messages']), 2)
        self.assertEqual(arms['no_previews']['messages'][1:-1], self.payload['messages'][1:-1])
        self.assertEqual(arms['no_previews']['messages'][-1]['content'], self.reference + ablation.BOUNDARY + self.question)

    def test_oracle_preserves_source_binding(self):
        content = self.arms()['oracle_unit']['messages'][-1]['content']
        self.assertEqual(content, self.block + ablation.BOUNDARY + self.question)
        self.assertNotIn(self.preview, content)

    def test_misbound_question_or_source_refused(self):
        with self.assertRaises(ValueError):
            ablation.variants(self.payload, 'Wrong question', self.unit)
        with self.assertRaises(ValueError):
            ablation.variants(self.payload, self.question, {'text': 'Wrong source'})

    def test_batch_encoding_field_count_cannot_be_used_as_token_count(self):
        class MappingTokenizer:
            def apply_chat_template(self, messages, tokenize, **kwargs):
                return {'input_ids': [1]*13, 'attention_mask': [1]*13} if tokenize else 'rendered text'
            def encode(self, text, add_special_tokens):
                self.text = text
                return list(range(13))
        tok = MappingTokenizer()
        ids = ablation.prompt_token_ids(tok, self.payload)
        self.assertEqual(len(ids), 13)
        self.assertEqual(tok.text, 'rendered text')

    def test_unexpected_renderer_type_refused(self):
        class BadTokenizer:
            def apply_chat_template(self, *args, **kwargs):
                return {'input_ids': [1, 2, 3]}
        with self.assertRaises(ValueError):
            ablation.prompt_token_ids(BadTokenizer(), self.payload)

    def test_batched_token_list_is_not_a_flat_prompt(self):
        class BatchedTokenizer:
            def apply_chat_template(self, *args, **kwargs):
                return 'rendered'
            def encode(self, *args, **kwargs):
                return [[1, 2, 3]]
        with self.assertRaises(ValueError):
            ablation.prompt_token_ids(BatchedTokenizer(), self.payload)


if __name__ == '__main__':
    unittest.main()
