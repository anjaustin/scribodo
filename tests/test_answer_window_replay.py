"""Window-replay arms and scoring (tools/answer_window_replay.py); no model, no server."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_window_replay', ROOT / 'tools/answer_window_replay.py')
replay = importlib.util.module_from_spec(spec); spec.loader.exec_module(replay)

NOTICE = 'The supplied manuscript passages do not give that. An answer was withheld because it is not stated in the verified passages.\n\nVerified passages:\nSource 1 (part01.md): "only three miles."'
ABSTAIN = 'The supplied manuscript passages do not give that. An answer was not found within the provided excerpts.'
ANSWER = 'Source 1 (part01.md): **"only three miles."** So the walk is three miles.'


def request(*turns):
    return {'messages': [{'role': 'system', 'content': 'composer instruction'}] + [{'role': r, 'content': c} for r, c in turns],
            'seed': 42, 'temperature': 0.7, 'stream': True}


class WindowReplayTests(unittest.TestCase):
    def test_refusals_are_recognised_and_answers_are_not(self):
        for text in (NOTICE, ABSTAIN, 'No verified passage was found in the supplied manuscript passages.',
                     "I don't have a verified passage from the Arizona chapters that names the horse.",
                     'The supplied passages do not contain that detail.'):
            self.assertTrue(replay.is_refusal(text), text[:40])
        for text in (ANSWER, 'Got it, noted for later.', '', None):
            self.assertFalse(replay.is_refusal(text))

    def test_windows_without_a_refusal_are_skipped(self):
        self.assertIsNone(replay.variants(request(('user', 'How far?'), ('assistant', ANSWER), ('user', 'How old?'))))

    def test_four_arms_change_only_the_history(self):
        req = request(('user', 'Q1'), ('assistant', NOTICE), ('user', 'Q2'), ('assistant', ABSTAIN), ('user', 'Q3'))
        refusals, arms = replay.variants(req, marker='(none)')
        self.assertEqual(refusals, [2, 4])
        self.assertEqual(set(arms), {'recorded', 'marker', 'drop_turn', 'drop_pair'})
        for arm in arms.values():
            self.assertIs(arm['stream'], False)
            self.assertEqual(arm['seed'], 42)
            self.assertEqual(arm['messages'][0]['content'], 'composer instruction')
            self.assertEqual(arm['messages'][-1]['content'], 'Q3')
        self.assertEqual([m['content'] for m in arms['recorded']['messages'][1:]], ['Q1', NOTICE, 'Q2', ABSTAIN, 'Q3'])
        self.assertEqual([m['content'] for m in arms['marker']['messages'][1:]], ['Q1', '(none)', 'Q2', '(none)', 'Q3'])
        self.assertEqual([m['content'] for m in arms['drop_turn']['messages'][1:]], ['Q1', 'Q2', 'Q3'])
        self.assertEqual([m['content'] for m in arms['drop_pair']['messages'][1:]], ['Q3'])
        self.assertIs(req['stream'], True)  # the captured request is untouched

    def test_scoring_counts_designated_passages_and_refusals(self):
        case = {'passages': ['only three miles', 'about twenty-seven']}
        self.assertEqual(replay.score(ANSWER, case), {'refusal': False, 'passages_present': 1, 'passages': 2})
        self.assertEqual(replay.score(ABSTAIN, case), {'refusal': True, 'passages_present': 0, 'passages': 2})
        self.assertEqual(replay.score('Only Three Miles, she said', None), {'refusal': False, 'passages_present': 0, 'passages': 0})
        self.assertEqual(replay.normalize('“Twenty-seven,” said Elizabeth’s friend.'), 'twenty seven said elizabeth s friend')


if __name__ == '__main__':
    unittest.main()
