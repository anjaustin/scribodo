"""Audit controls use synthetic captures, never services or inference."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('audit', ROOT / 'tools/answer_live_audit.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class AuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.capture = self.root / 'capture'; self.capture.mkdir()
        self.product = self.root / 'product'; self.product.mkdir()
        self.units, self.messages = [], []
        for index, (document, passage) in enumerate(audit.PASSAGES):
            self.units.append({'unit_id': f'{index+1:064x}', 'document_name': document, 'text': passage,
                               'text_hash': hashlib.sha256(passage.encode()).hexdigest(), 'scope_id': 'scope',
                               'generation': 3, 'source_kind': 'document', 'sequence': None,
                               'source_hash_current': True, 'source_bytes_current': True})
        for index in range(7):
            unit = self.units[min(index, 5)]
            candidate = {'unit_id': unit['unit_id'], 'text_hash': unit['text_hash']}
            self.save(self.capture / f'retrieval-{index}.json', {'query': f'question {index}', 'before': 400,
                      'scope_id': 'scope', 'generation': 3, 'lexical': [candidate], 'semantic': [candidate]})
            self.save(self.capture / f'request-{index}.json', {'messages': [{'role': 'user', 'content': unit['text']}]})
            self.save(self.capture / f'outcome-{index}.json', {'capture_complete': True})
            (self.capture / f'content-{index}.txt').write_text('reply')
            event = {'choices': [{'delta': {'content': 'reply'}, 'finish_reason': 'stop'}]}
            (self.capture / f'stream-{index}.sse').write_text('data: '+json.dumps(event)+'\n')
            self.messages.extend([{'seq': 403+2*index, 'content': f'question {index}'},
                                  {'seq': 404+2*index, 'content': 'reply',
                                   'memory_sources': f"| unit={unit['unit_id']} | bytes=0+1"}])
        self.save(self.product / 'units.json', self.units)
        self.save(self.product / 'messages.json', self.messages)
        (self.product / 'app.log').write_text('synthetic')

    @staticmethod
    def save(path, value):
        path.write_text(json.dumps(value))

    def run_audit(self):
        return audit.audit(self.capture, self.product)

    def test_clean_control_and_exact_passages(self):
        result = self.run_audit()
        self.assertEqual(result['integrity_failures'], [])
        self.assertTrue(all(c['designated']['admitted'] for c in result['cases'][:6]))

    def test_tampered_source_and_stream_are_detected(self):
        self.units[0]['text_hash'] = '0'*64
        self.save(self.product / 'units.json', self.units)
        (self.capture / 'content-1.txt').write_text('tampered')
        failures = self.run_audit()['integrity_failures']
        self.assertIn('source integrity', [f['failure'] for f in failures])
        self.assertIn('SSE content accumulation', [f['failure'] for f in failures])

    def test_future_chat_and_missing_locator_are_detected(self):
        self.units[0].update(source_kind='chat', sequence=400)
        self.save(self.product / 'units.json', self.units)
        path = self.capture / 'retrieval-1.json'
        trace = json.loads(path.read_text()); trace['semantic'][0]['unit_id'] = 'f'*64
        self.save(path, trace)
        failures = self.run_audit()['integrity_failures']
        self.assertGreaterEqual(sum(f['failure'] == 'locator binding/cutoff' for f in failures), 3)

    def test_phrase_presence_is_not_designated_admission(self):
        self.messages[3]['memory_sources'] = 'four or five thousand a year, unrelated source'
        self.save(self.product / 'messages.json', self.messages)
        case = self.run_audit()['cases'][1]
        self.assertFalse(case['designated']['admitted'])
        self.assertTrue(case['designated']['passage_in_final_request'])


if __name__ == '__main__':
    unittest.main()
