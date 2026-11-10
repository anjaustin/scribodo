"""Live diagnostic instrumentation invariants; no models or services."""
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
with patch.object(sys, 'path', [str(ROOT / 'tools'), *sys.path]):
    spec = importlib.util.spec_from_file_location('answer_live_capture', ROOT / 'tools/answer_live_capture.py')
    live = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(live)


class LiveInstrumentationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.chat = (ROOT / 'src/llm/chat.c').read_text()
        cls.planner = (ROOT / 'src/llm/context_planner.c').read_text()

    def test_request_builders_and_model_instructions_are_unchanged(self):
        instrumented = live.instrument_chat(self.chat)
        persona = self.chat[self.chat.index('static const char *k_persona ='):self.chat.index('struct ChatSession {')]
        builders = self.chat[self.chat.index('static bool request_message('):self.chat.index('/* ---- streaming')]
        self.assertIn(persona, instrumented)
        self.assertIn(builders, instrumented)
        self.assertNotIn('DIAGNOSTIC REPLAY', instrumented)
        self.assertNotIn('frozen reply', instrumented)

    def test_actual_provider_call_is_preserved_and_stream_precedes_parsing(self):
        instrumented = live.instrument_chat(self.chat)
        start = self.chat.index('    bool ok = s->via_remote ? remote_request(')
        end = self.chat.index('    bool cancelled =', start)
        self.assertIn(self.chat[start:end], instrumented)
        self.assertLess(instrumented.index('fwrite(line, 1, len'), instrumented.index('int rc = chat_parse_sse_line'))
        self.assertIn('O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW, 0600', instrumented)
        self.assertIn('S_ISDIR(status.st_mode)', instrumented)
        self.assertIn('status.st_uid == getuid() && !(status.st_mode & 077)', instrumented)

    def test_planner_searches_and_fusion_are_unchanged(self):
        instrumented = live.instrument_planner(self.planner)
        start = self.planner.index('    if (!stopped(cancel)) plan.lexical_ok =')
        end = self.planner.index('    Ranked ranked[MEMORY_CANDIDATES]', start)
        self.assertIn(self.planner[start:end], instrumented)
        # Everything from fusion through admission/application stays byte-equal.
        self.assertIn(self.planner[end:], instrumented)
        self.assertIn('"before", (double)older_than', instrumented)
        self.assertIn('"text_hash", list[i].text_hash', instrumented)

    def test_changed_or_ambiguous_anchors_refuse(self):
        with self.assertRaises(ValueError):
            live.replace_once('not an anchor', 'required anchor', 'replacement')
        with self.assertRaises(ValueError):
            live.replace_once('anchor anchor', 'anchor', 'replacement')


if __name__ == '__main__':
    unittest.main()
