"""Per-part claim arm: deterministic parts only; no model, no server. Pins the
single arm's strings to the production claim call in src/llm/answer_pipeline.c."""
import importlib.util
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_part_claims', ROOT / 'tools/answer_part_claims.py')
pc = importlib.util.module_from_spec(spec); spec.loader.exec_module(pc)
SOURCE = (ROOT / 'src/llm/answer_pipeline.c').read_text()


def c_string(name):
    """The value of `static const char name[] = "..." "...";` with adjacent literals joined and \\" unescaped."""
    m = re.search(r'static const char ' + name + r'\[\] =\s*((?:"(?:[^"\\]|\\.)*"\s*)+);', SOURCE)
    assert m, name
    return ''.join(s.replace('\\"', '"') for s in re.findall(r'"((?:[^"\\]|\\.)*)"', m[1]))


class PartClaimsTests(unittest.TestCase):
    def test_single_arm_strings_are_the_production_claim_call(self):
        self.assertEqual(pc.CLAIM_READER, c_string('k_claim_reader'))
        self.assertEqual(pc.CLAIM_TASK, c_string('k_claim_task'))
        self.assertEqual(pc.CLAIM_SCHEMA, json.loads(c_string('k_claim_schema')))
        self.assertIn('cJSON_AddNumberToObject(root, "max_tokens", %d)' % pc.CLAIM_MAX_TOKENS, SOURCE)

    def test_single_request_has_the_production_shape(self):
        r = pc.single_request('Q?', 'R.')
        self.assertEqual([m['role'] for m in r['messages']], ['system', 'user'])
        self.assertEqual(r['messages'][1]['content'], 'QUESTION:\nQ?\n\nREPLY:\nR.\n\n' + pc.CLAIM_TASK)
        self.assertEqual((r['temperature'], r['max_tokens'], r['stream'], r['cache_prompt']), (0, 96, False, False))
        self.assertFalse(r['chat_template_kwargs']['enable_thinking'])
        self.assertEqual(r['response_format']['json_schema']['schema'], pc.CLAIM_SCHEMA)

    def test_parts_request_lists_the_parts_and_fixes_the_answer_count(self):
        r = pc.parts_request('A, and B?', 'R.', ['A?', 'B?'])
        self.assertTrue(r['messages'][1]['content'].endswith('PARTS:\n1. A?\n2. B?'))
        schema = r['response_format']['json_schema']['schema']
        self.assertEqual((schema['properties']['answers']['minItems'], schema['properties']['answers']['maxItems']), (2, 2))

    def test_names_skip_sentence_starts_honorifics_and_possessives(self):
        self.assertEqual(pc.names("What fortune do Mr. Bingley's sisters have?"), {'bingley'})
        self.assertEqual(pc.names('Her daughter, Miss de Bourgh, will have a fortune. She smiled.'), {'bourgh'})
        self.assertEqual(pc.names('I told you Lydia is seventeen; I was wrong.'), {'lydia'})

    def test_analysis_flags_a_quantity_part_answered_without_a_number_and_new_names(self):
        a = pc.analyze("What fortune do Mr. Bingley's sisters have?", "Miss de Bourgh has 'a very large fortune'",
                       "What fortune do Mr. Bingley's sisters have, and what is Mr. Bingley's own inheritance said to be?", [])
        self.assertEqual((a['quantity_question'], a['has_number'], a['new_names'], a['empty']), (True, False, ['bourgh'], False))
        b = pc.analyze('How far is the walk?', 'three miles', 'How far is the walk?', [])
        self.assertEqual((b['quantity_question'], b['has_number'], b['new_names']), (True, True, []))
        c = pc.analyze('what did the other draft say?', 'five miles, per Lydia', 'Q?', ['Note: Lydia walked five miles.'])
        self.assertEqual((c['quantity_question'], c['has_number'], c['new_names']), (False, True, []))
        self.assertTrue(pc.analyze('How old is she?', '', 'How old is she?', [])['empty'])
        self.assertFalse(pc.has_number('1. The supplied text does not say; Source 2 mentions nothing.'))
        self.assertTrue(pc.has_number('1. about twenty-seven, per Source 2'))

    def test_replies_take_only_gated_questions_in_request_order(self):
        import tempfile
        fixture = {'cases': [{'id': 'walk', 'question': 'How far?'}]}
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 'request-0.json').write_text(json.dumps({'messages': [{'role': 'user', 'content': 'Note: five miles.'}]}))
            (d / 'content-0.txt').write_text('noted')
            (d / 'request-1.json').write_text(json.dumps({'messages': [{'role': 'user', 'content': 'Note: five miles.'},
                                                                        {'role': 'assistant', 'content': 'noted'},
                                                                        {'role': 'user', 'content': 'How far?'}]}))
            (d / 'content-1.txt').write_text('three miles')
            rows = pc.replies(d, fixture)
        self.assertEqual(rows, [(d.name, 'walk', 'How far?', 'three miles', ['Note: five miles.'])])


if __name__ == '__main__':
    unittest.main()
