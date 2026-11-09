"""Diagnostic integrity tests; no models, SQL, Qdrant or author data."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_bedrock_probe', ROOT / 'tools/answer_bedrock_probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
BOUNDARY = "\nAuthor's current request (not part of the retrieved sources):\n"
QUESTION = "What is Mr. Bingley's income said to be when he first arrives in the neighbourhood?"
OPENING = 'A single man of large fortune; four or five thousand a year.'


def source(number, name, revision, body):
    return f'--- SOURCE {number} | document-name (untrusted data)="{name}" | revision={revision} | bytes=0+{len(body.encode())} ---\n{body}\n--- END SOURCE ---'


class ProbeIntegrityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='answer-probe-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.capture = self.root / 'capture'
        self.capture.mkdir()
        self.transcript = [{'seq': 402, 'role': 'user', 'content': 'Prior archive turn'}]
        for i in range(7):
            self.transcript += [{'seq': 403+2*i, 'role': 'user', 'content': QUESTION if i == 1 else f'Question {i}'},
                                {'seq': 404+2*i, 'role': 'assistant', 'content': f'Frozen reply {i}'}]
        (self.capture / 'transcript.json').write_text(json.dumps(self.transcript))
        self.opening = source(1, 'part01.md', 'r-original-opening', OPENING)
        blocks = [source(i, f'part0{i}.md', f'r-{i}', f'Unrelated scene {i}.') for i in range(1, 8)]
        blocks[2] = source(3, 'part08.md', 'r-later-scene', 'He has four or five thousand a year, and very likely more.')
        blocks[3] = source(4, 'part01.md', 'r-darcy', 'Mr. Darcy has ten thousand a year.')
        self.request = {'messages': [{'role': 'system', 'content': 'Creative partner'},
                                     {'role': 'user', 'content': 'Earlier question'},
                                     {'role': 'assistant', 'content': 'Prior reply'},
                                     {'role': 'user', 'content': '\n\n'.join(blocks) + BOUNDARY + 'DOCUMENT HEADS\n' + QUESTION}],
                        'max_tokens': 2048, 'temperature': .7, 'cache_prompt': True, 'stream': True}
        (self.capture / 'request-1.json').write_text(json.dumps(self.request))
        first = copy.deepcopy(self.request)
        first['messages'][-1]['content'] = self.opening + BOUNDARY + 'Question 0'
        (self.capture / 'request-0.json').write_text(json.dumps(first))
        # Legacy tool inputs are present for the negative-control run against
        # the old probe; the current tool must not depend on these mutable files.
        legacy = self.root / 'build/product-memory-use/after/messages.json'
        legacy.parent.mkdir(parents=True)
        legacy.write_text(json.dumps(self.transcript))
        manuscript = self.root / 'build/manuscripts/pride-and-prejudice/parts/part01.md'
        manuscript.parent.mkdir(parents=True)
        manuscript.write_text(OPENING)

    def test_oracle_body_keeps_its_actual_binding(self):
        with patch.object(probe, 'ROOT', self.root):
            for phase, arm in [('evidence', 'packet_opening_instead'), ('attribution', 'packet_correct_binding')]:
                with self.subTest(phase=phase):
                    _, variants = probe.cases(self.capture, 1, phase)
                    text = dict(variants)[arm]['messages'][-1]['content']
                    actual = source(3, 'part01.md', 'r-original-opening', OPENING)
                    self.assertIn(actual, text)
                    self.assertNotIn('r-later-scene', text)

    def test_question_is_frozen_not_read_from_mutable_legacy_output(self):
        legacy = self.root / 'build/product-memory-use/after/messages.json'
        changed = copy.deepcopy(self.transcript)
        changed[3]['content'] = 'A different later-run question'
        legacy.write_text(json.dumps(changed))
        with patch.object(probe, 'ROOT', self.root):
            _, variants = probe.cases(self.capture, 1)
        self.assertTrue(all(req['messages'][-1]['content'].endswith(QUESTION) for _, req in variants))

    def test_component_comparisons_preserve_original_and_change_only_their_factor(self):
        original, variants = probe.cases(self.capture, 1)
        arms = dict(variants)
        self.assertEqual(original, self.request)
        self.assertEqual(json.loads((self.capture / 'request-1.json').read_text()), self.request)
        self.assertEqual(arms['full_cold']['messages'], self.request['messages'])
        cached = copy.deepcopy(arms['full_cold'])
        cached['cache_prompt'] = True
        self.assertEqual(cached, arms['full_cached'])
        sampled = copy.deepcopy(arms['full_cold'])
        sampled['temperature'] = self.request['temperature']
        self.assertEqual(sampled, arms['full_sampled'])
        self.assertEqual(len(arms['no_history']['messages']), 2)
        self.assertNotIn('DOCUMENT HEADS', arms['no_document_heads']['messages'][-1]['content'])

    def test_missing_frozen_transcript_refuses(self):
        (self.capture / 'transcript.json').unlink()
        with self.assertRaises((ValueError, FileNotFoundError)):
            probe.cases(self.capture, 1)

    def test_invalid_cli_limits_refuse_before_creating_outputs(self):
        for option, value in [('--index', '-1'), ('--max-tokens', '0'), ('--max-tokens', '8193')]:
            with self.subTest(option=option, value=value):
                out = self.root / f'refused-{value}'
                run = subprocess.run([sys.executable, str(ROOT / 'tools/answer_bedrock_probe.py'),
                                      '--output', str(out), option, value], capture_output=True, text=True)
                self.assertEqual(run.returncode, 2)
                self.assertFalse(out.exists())

    def test_private_socket_directory_removed_after_failure(self):
        with self.assertRaisesRegex(RuntimeError, 'injected'):
            with probe.private_socket() as sock:
                directory = sock.parent
                self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
                sock.write_text('stand-in for owned socket')
                raise RuntimeError('injected')
        self.assertFalse(directory.exists())

    def test_server_stopped_after_request_failure(self):
        child = Mock()
        child.poll.return_value = None
        child.wait.side_effect = [subprocess.TimeoutExpired('fixture', 20), 0]
        def response(sock, endpoint, data=None):
            if endpoint == '/health':
                return {'status': 'ok'}
            raise RuntimeError('injected request failure')
        with patch.object(probe.subprocess, 'Popen', return_value=child), patch.object(probe, 'call', side_effect=response):
            with self.assertRaisesRegex(RuntimeError, 'injected request failure'):
                probe.run_server(self.root, ['fixture'], Path('unused.sock'), [])
        child.terminate.assert_called_once()
        child.kill.assert_called_once()
        self.assertEqual(child.wait.call_count, 2)


if __name__ == '__main__':
    unittest.main()
