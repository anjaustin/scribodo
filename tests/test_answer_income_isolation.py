"""Income selection/execution isolation: packet construction invariants, no models."""
import importlib.util
import json
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
# Heavy helpers (tokenizer, model paths) are stubbed; only pure packet code runs.
stubs = {
    'answer_bedrock_probe': types.SimpleNamespace(private_socket=None, run_server=None),
    'answer_live_ablation': types.SimpleNamespace(BOUNDARY="\nAuthor's current request (not part of the retrieved sources):\n", prompt_token_ids=None),
    'answer_live_audit': types.SimpleNamespace(PASSAGES=(('part01.md', 'opening'), ('part01.md', 'four or five thousand a year'))),
    'answer_manifold_probe': types.SimpleNamespace(SENTENCE='\nAnswer in one sentence.'),
    'memory_causal_parity': types.SimpleNamespace(GGUF=None, GGUF_SHA=None, ROOT=ROOT, sha=None, tokenizer=None),
}
with patch.dict(sys.modules, stubs):
    spec = importlib.util.spec_from_file_location('answer_income_isolation', ROOT / 'tools/answer_income_isolation.py')
    iso = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(iso)

PREFIX = 'document-name (untrusted data)="part01.md" | document=d-1 | revision=r-1 | SHA256=' + 'a'*64


def block(number, unit, start, text, prefix=PREFIX):
    return f"--- SOURCE {number} | {prefix} | unit={unit} | bytes={start}+{len(text.encode())} ---\n{text}\n--- END SOURCE ---"


class IncomeIsolationTests(unittest.TestCase):
    def setUp(self):
        self.units = {
            '1'*64: {'unit_id': '1'*64, 'text': 'Bingley inherited a hundred thousand pounds.', 'document_name': 'part01.md', 'revision_id': 'r-1', 'byte_start': 100},
            '7'*64: {'unit_id': '7'*64, 'text': 'Some other passage from part07.', 'document_name': 'part07.md', 'revision_id': 'r-7', 'byte_start': 5},
            '3'*64: {'unit_id': '3'*64, 'text': 'The last admitted block.', 'document_name': 'part01.md', 'revision_id': 'r-1', 'byte_start': 900},
        }
        self.designated = {'unit_id': 'd'*64, 'text': 'A single man of large fortune; four or five thousand a year.',
                           'document_name': 'part01.md', 'revision_id': 'r-1', 'byte_start': 1800}
        other_prefix = PREFIX.replace('part01.md', 'part07.md').replace('r-1', 'r-7')
        reference = 'Retrieved excerpts.\n\n' + '\n'.join([
            block(1, '1'*64, 100, self.units['1'*64]['text']),
            block(2, '7'*64, 5, self.units['7'*64]['text'], other_prefix),
            block(3, '3'*64, 900, self.units['3'*64]['text']),
        ]) + '\n'
        self.question = 'What is his income?'
        self.payload = {'messages': [{'role': 'system', 'content': 'persona'}, {'role': 'user', 'content': 'earlier'},
                                     {'role': 'assistant', 'content': 'reply'},
                                     {'role': 'user', 'content': reference + iso.BOUNDARY + 'attachment preview\n' + self.question}],
                        'seed': 42, 'temperature': 0.3, 'max_tokens': 2048}

    def test_swap_replaces_only_the_chosen_block_and_carries_the_real_locator(self):
        arms, variants = iso.build_arms(self.payload, self.question, self.designated, '1'*64, self.units)
        names = [n for n, _ in arms]
        self.assertEqual(names, ['baseline', 'baseline_one_sentence', 'swap_quoted', 'swap_quoted_one_sentence', 'swap_last', 'swap_last_one_sentence'])
        checks = iso.verify(arms, variants, self.designated, '1'*64, self.payload)
        self.assertTrue(all(c['ok'] for c in checks.values()))
        swapped = dict(arms)['swap_quoted']['messages'][-1]['content']
        self.assertIn(f"--- SOURCE 1 | {PREFIX} | unit={'d'*64} | bytes=1800+{len(self.designated['text'].encode())} ---\n{self.designated['text']}\n--- END SOURCE ---", swapped)
        self.assertNotIn('1'*64, swapped)
        self.assertIn('7'*64, swapped); self.assertIn('3'*64, swapped)
        self.assertTrue(swapped.endswith(iso.BOUNDARY + 'attachment preview\n' + self.question))
        for _, request in arms:
            self.assertEqual(request['messages'][1:-1], self.payload['messages'][1:-1])
            self.assertEqual((request['temperature'], request['cache_prompt'], request['max_tokens'], request['stream']), (0, False, 2048, False))

    def test_refuses_when_designated_already_present_or_quoted_block_missing(self):
        present = json.loads(json.dumps(self.payload))
        present['messages'][-1]['content'] = present['messages'][-1]['content'].replace('The last admitted block.', self.designated['text'])
        with self.assertRaises(ValueError):
            iso.build_arms(present, self.question, self.designated, '1'*64, self.units)
        with self.assertRaises(ValueError):
            iso.build_arms(self.payload, self.question, self.designated, 'e'*64, self.units)

    def test_refuses_header_prefix_from_another_document(self):
        with self.assertRaises(ValueError):
            iso.build_arms(self.payload, self.question, self.designated, '7'*64, self.units)

    def test_verify_detects_a_tampered_history(self):
        arms, variants = iso.build_arms(self.payload, self.question, self.designated, '1'*64, self.units)
        arms[2][1]['messages'][1]['content'] = 'changed'
        with self.assertRaises(ValueError):
            iso.verify(arms, variants, self.designated, '1'*64, self.payload)


if __name__ == '__main__':
    unittest.main()
