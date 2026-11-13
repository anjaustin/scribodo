"""Relation arm: the deterministic check, model-free (tools/answer_relation_arm.py)."""
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_relation_arm', ROOT / 'tools/answer_relation_arm.py')
ra = importlib.util.module_from_spec(spec); spec.loader.exec_module(ra)

SPANS = ['I, the daughter of Mors Kajak, son of Tardos Mors, have listened without anger,']
QUESTION = 'Earlier I told you Dejah Thoris is the daughter of Tardos Mors; I was wrong, the text says otherwise. Whose daughter does she say she is?'


class RelationArmTests(unittest.TestCase):
    def test_prose_excludes_quotations_and_block_quotes(self):
        reply = 'Source 2 begins:\n\n> "I, the daughter of Mors Kajak"\n\nShe says "daughter of Mors Kajak" and her father is Tardos Mors.'
        self.assertNotIn('Mors Kajak"', ra.prose(reply))
        self.assertIn('her father is Tardos Mors', ra.prose(reply))
        self.assertNotIn('> ', ra.prose(reply))

    def test_the_recorded_misreading_is_flagged_and_its_correct_denial_is_not(self):
        reply = ('This states that the speaker is the daughter of Mors Kajak and son of Tardos Mors — i.e., her father is Tardos Mors '
                 'and her mother is Mors Kajak. So according to this passage, she is **not** the daughter of Tardos Mors; she is the '
                 'daughter of Mors Kajak, son of Tardos Mors.')
        rows = ra.check(reply, SPANS, [], QUESTION)
        flagged = [r['phrase'] for r in rows if r['flag']]
        self.assertEqual(flagged, ['her father is Tardos Mors', 'her mother is Mors Kajak'])
        negated = [r['phrase'] for r in rows if r['negated']]
        self.assertIn('daughter of Tardos Mors', negated)
        self.assertTrue(all(r['supported'] for r in rows if r['phrase'] in ('daughter of Mors Kajak', 'son of Tardos Mors')))

    def test_possessive_is_supported_by_the_of_form_and_sentence_starters_are_not_names(self):
        rows = ra.check("Mr. Bingley's sisters have twenty thousand pounds. The estate is Netherfield.",
                        ['They were rather handsome; had a fortune of twenty thousand pounds; the sisters of Mr. Bingley'], [], 'What fortune?')
        by = {r['phrase']: r for r in rows}
        self.assertTrue(by["Mr. Bingley's sisters"]['supported'])
        self.assertNotIn('The estate is Netherfield', by)  # "The" is not a name
        rows = ra.check("Mr. Bingley's estate is large.", ['Mr. Bingley inherited property'], [], 'q')
        self.assertEqual([r['phrase'] for r in rows if r['flag']], ["Mr. Bingley's estate"])

    def test_inheritance_is_supported_by_inherited_and_phrases_carry_their_class(self):
        span = ['Mr. Bingley inherited property to the amount of nearly a hundred thousand pounds from his father']
        rows = ra.check("Mr. Bingley's inheritance is nearly a hundred thousand pounds. Mr. Bingley's estate is Netherfield Park.", span, [], 'q')
        self.assertEqual([(r['phrase'], r['class'], r['flag']) for r in rows],
                         [("Mr. Bingley's inheritance", 'possession', False), ("Mr. Bingley's estate", 'possession', True),
                          ("Mr. Bingley's estate is Netherfield Park", 'possession', True)])
        self.assertEqual([r['flag'] for r in ra.check("Mr. Darcy's inheritance is large.", span, [], 'q')], [True])
        # The past tense is checked whole like the present (journal/redteam-loose_ends.md).
        rows = ra.check("Mr. Bingley's inheritance was Netherfield Park. Mr. Bingley was the heir of Mr. Darcy.", span, [], 'q')
        self.assertEqual([(r['phrase'], r['flag']) for r in rows],
                         [("Mr. Bingley's inheritance", False), ("Mr. Bingley's inheritance was Netherfield Park", True),
                          ('heir of Mr. Darcy', True), ("Mr. Bingley was the heir of Mr. Darcy", True)])
        self.assertEqual(ra.phrase_class("Elizabeth's father is Mr. Bennet"), 'kinship')
        self.assertEqual(ra.phrase_class("daughter of Mr. Bennet"), 'kinship')

    def test_author_turns_and_the_question_support_a_phrase(self):
        rows = ra.check("Lydia's age is seventeen.", [], ['Note for later: Lydia, my youngest, is seventeen.'], 'How old?')
        self.assertEqual(rows, [])  # "age" is not in the closed kin list
        rows = ra.check('Her father is Tardos Mors.', [], [], 'Earlier I told you her father is Tardos Mors; whose daughter is she?')
        self.assertFalse(rows[0]['flag'])

    def test_a_possessive_subject_is_checked_whole_and_an_adjective_name_is_not_a_subject(self):
        rows = ra.check("Elizabeth's father is Mr. Bennet. The father is Mr. Bennet. The youngest Bennet daughter is Lydia.", [], [], 'q')
        self.assertEqual([(r['phrase'], r['flag']) for r in rows], [("Elizabeth's father", True), ("Elizabeth's father is Mr. Bennet", True)])
        rows = ra.check("Elizabeth's father is Mr. Bennet.", ["Elizabeth's father, Mr. Bennet, was a man of wit."], [], 'q')
        self.assertEqual([(r['phrase'], r['flag']) for r in rows], [("Elizabeth's father", False), ("Elizabeth's father is Mr. Bennet", True)])
        rows = ra.check("Elizabeth's father is Mr. Bennet.", [], [], "Is Elizabeth's father is Mr. Bennet?")
        self.assertEqual([r['flag'] for r in rows], [False, False])

    def test_third_novel_rules(self):
        """A question the composer asks back states nothing; a negation reaches its own clause; nephew is kin; a
        possessive continues the name (journal/answer-cycle4_heights.md)."""
        spans = ['No; he is the late Mrs. Linton’s nephew.']
        rows = ra.check("Hareton Earnshaw is Heathcliff’s nephew, not his son. Who is Hareton Earnshaw's father?", spans, [], 'Whose son is Hareton?')
        self.assertEqual([(r['phrase'], r['negated'], r['flag']) for r in rows], [('Heathcliff’s nephew', False, True)])
        rows = ra.check("He is the late Mrs. Linton's nephew, not Heathcliff's son.", spans, [], 'q')
        self.assertEqual([(r['phrase'], r['negated'], r['flag']) for r in rows], [("Mrs. Linton's nephew", False, False), ("Heathcliff's son", True, False)])
        rows = ra.check("She is the daughter of Mr. Bingley's cousin.", [], [], 'q')
        self.assertEqual([(r['phrase'], r['flag']) for r in rows], [("Mr. Bingley's cousin", True)])

    def test_visible_reply_is_what_the_product_checks(self):
        self.assertEqual(ra.visible('<think>her father is X</think>\n\nShe is Y.'), 'She is Y.')
        self.assertEqual(ra.visible('reasoning here</think>\nShe is Y.'), 'She is Y.')
        # llm_split_thinking treats any </think> not opened at the start as an orphan close: what precedes it was reasoning.
        self.assertEqual(ra.visible('She is Y. <think>literal</think> tag stays'), 'tag stays')
        self.assertEqual(ra.visible('<think>still streaming'), '')

    def test_recorded_fixture_agrees_with_this_check(self):
        """The parity fixture the C check is tested against was written by this arm; re-checking it must reproduce it."""
        path = ROOT / 'tests/fixtures/answer_relation/replies.json'
        rows = json.loads(path.read_text())['replies']
        self.assertGreaterEqual(len(rows), 60)
        flagged = 0
        for row in rows:
            got = ra.check(row['reply'], row['spans'], row['turns'], row['question'])
            flags = [r['phrase'] for r in got if r['flag']]
            self.assertEqual(len(got), row['checked'], row['question'])
            self.assertEqual(flags[0] if flags else '', row['unsupported'], row['question'])
            flagged += bool(flags)
        self.assertEqual(flagged, 21)  # 230 rows from 23 recorded runs (journal/answer-cycle4_relation_split.md)


if __name__ == '__main__':
    unittest.main()
