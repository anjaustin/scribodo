"""Coreference-at-ingestion arm: the deterministic resolution rule only; no model, no server."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('answer_coref_arm', ROOT / 'tools/answer_coref_arm.py')
ca = importlib.util.module_from_spec(spec); spec.loader.exec_module(ca)

TEXT = ('Mr. Bingley came. His sisters were fine women. They had a fortune of twenty thousand pounds; '
        'he admired them. Their manners are not equal to his. Miss Bingley kept her house; it stood near.')
TAGS = {'Mr.': 'NNP', 'Bingley': 'NNP', 'Miss': 'NNP', 'His': 'PRP$', 'Their': 'PRP$', 'her': 'PRP$', 'his': 'PRP',
        'They': 'PRP', 'he': 'PRP', 'them': 'PRP', 'it': 'PRP'}
DEPS = {'They': 'nsubj', 'he': 'nsubj', 'His': 'poss', 'Their': 'poss', 'her': 'poss', 'it': 'nsubj'}
HEAD_DEPS = {'His': 'nsubj', 'Their': 'nsubj', 'her': 'dobj'}


def tokens(text=TEXT):
    out, at = [], 0
    for word in text.replace(';', ' ;').replace('.', ' .').split():
        at = text.index(word, at)
        out.append(ca.Token(at, at + len(word), word, TAGS.get(word, 'NN'), DEPS.get(word, ''), HEAD_DEPS.get(word, '')))
        at += len(word)
    return out


def mention(text, needle, cluster, occurrence=0):
    import re
    at = [m.start() for m in re.finditer(r'(?<!\w)' + re.escape(needle) + r'(?!\w)', text)][occurrence]
    return ca.Mention(at, at + len(needle), needle, cluster)


class CorefArmTests(unittest.TestCase):
    def test_chunks_group_consecutive_units_under_the_core_size_with_a_bounded_context_prefix(self):
        spans = [(0, 100), (100, 250), (250, 400), (400, 900), (900, 1000)]
        self.assertEqual(ca.chunks(spans, core_chars=300, context_chars=120),
                         [(0, 0, 250), (130, 250, 400), (280, 400, 900), (780, 900, 1000)])
        self.assertEqual(ca.chunks([(0, 50)], core_chars=10, context_chars=5), [(0, 0, 50)])

    def test_representative_is_the_most_frequent_named_mention_else_the_earliest_nominal(self):
        toks = tokens()
        bingley = [mention(TEXT, 'Mr. Bingley', 0), mention(TEXT, 'His', 0), mention(TEXT, 'he', 0), mention(TEXT, 'his', 0)]
        text, m, named = ca.representative(bingley, toks)
        self.assertEqual((text, named), ('Mr. Bingley', True))
        sisters = [mention(TEXT, 'His sisters', 1), mention(TEXT, 'They', 1), mention(TEXT, 'them', 1), mention(TEXT, 'Their', 1)]
        text, m, named = ca.representative(sisters, toks)
        self.assertEqual((text, named), ('His sisters', False))
        self.assertEqual(ca.representative([mention(TEXT, 'it', 9)], toks), (None, None, False))

    def test_pronoun_kinds_follow_the_word_and_the_tag(self):
        toks = tokens()
        self.assertEqual(ca.pronoun_kind(mention(TEXT, 'His', 0), toks), 'possessive')
        self.assertEqual(ca.pronoun_kind(mention(TEXT, 'his', 0), toks), 'possessive')  # standalone "equal to his"
        self.assertEqual(ca.pronoun_kind(mention(TEXT, 'her', 0), toks), 'possessive')
        self.assertEqual(ca.pronoun_kind(mention(TEXT, 'They', 0), toks), 'plain')
        self.assertIsNone(ca.pronoun_kind(mention(TEXT, 'His sisters', 0), toks))
        self.assertIsNone(ca.pronoun_kind(mention(TEXT, 'Mr. Bingley', 0), toks))

    def test_replace_form_names_third_person_pronouns_and_nests_one_level(self):
        toks = tokens()
        mentions = [mention(TEXT, 'Mr. Bingley', 0), mention(TEXT, 'His', 0), mention(TEXT, 'he', 0), mention(TEXT, 'his', 0),
                    mention(TEXT, 'His sisters', 1), mention(TEXT, 'They', 1), mention(TEXT, 'them', 1), mention(TEXT, 'Their', 1),
                    mention(TEXT, 'Miss Bingley', 2), mention(TEXT, 'her', 2), mention(TEXT, 'it', 3)]
        out = ca.apply(TEXT, ca.replacements(mentions, toks, 'replace'))
        self.assertEqual(out, 'Mr. Bingley came. Mr. Bingley’s sisters were fine women. Mr. Bingley’s sisters had a fortune of '
                              'twenty thousand pounds; Mr. Bingley admired Mr. Bingley’s sisters. Mr. Bingley’s sisters’ manners '
                              'are not equal to Mr. Bingley’s. Miss Bingley kept Miss Bingley’s house; it stood near.')

    def test_annotate_form_keeps_every_original_byte_and_appends_the_name(self):
        toks = tokens()
        mentions = [mention(TEXT, 'Mr. Bingley', 0), mention(TEXT, 'His', 0), mention(TEXT, 'he', 0),
                    mention(TEXT, 'His sisters', 1), mention(TEXT, 'They', 1)]
        out = ca.apply(TEXT, ca.replacements(mentions, toks, 'annotate'))
        self.assertIn('They [Mr. Bingley’s sisters] had a fortune', out)
        self.assertIn('he [Mr. Bingley] admired', out)
        for word in TEXT.split():
            self.assertIn(word, out)

    def test_neuter_pronouns_are_rewritten_only_to_a_named_representative(self):
        toks = tokens()
        self.assertEqual(ca.replacements([mention(TEXT, 'her house', 5), mention(TEXT, 'it', 5)], toks, 'replace'), [])
        edits = ca.replacements([mention(TEXT, 'Miss Bingley', 5), mention(TEXT, 'it', 5)], toks, 'replace')
        self.assertEqual([e[2] for e in edits], ['Miss Bingley'])

    def test_a_capital_only_at_a_sentence_start_is_not_a_name(self):
        text = 'Vain indeed must be her attentions. Miss Bingley praised Vain again.'
        toks = [ca.Token(m.start(), m.end(), m.group(), 'NNP' if m.group()[0].isupper() else 'NN', '', '')
                for m in __import__('re').finditer(r"[\w']+|[.]", text)]
        self.assertEqual(ca.proper_vocabulary(toks), {'Bingley', 'Vain'})  # the second "Vain" is mid-sentence
        toks = [t for t in toks if t.start < text.index('Miss')]
        self.assertEqual(ca.proper_vocabulary(toks), set())
        self.assertFalse(ca.is_named(ca.Mention(0, 4, 'Vain', 0), toks))

    def test_named_representative_excludes_mentions_with_a_pronoun_or_comma_and_prefers_the_shortest(self):
        text = 'Her daughter, Miss de Bourgh, will have a fortune; Miss de Bourgh is plain.'
        import re
        toks = [ca.Token(m.start(), m.end(), m.group(), 'NNP' if m.group()[0].isupper() and m.group() != 'Her' else ('PRP$' if m.group() == 'Her' else 'NN'), '', '')
                for m in re.finditer(r"[\w']+|[.,;]", text)]
        ms = [ca.Mention(0, 28, 'Her daughter, Miss de Bourgh', 0), ca.Mention(51, 64, 'Miss de Bourgh', 0)]
        self.assertEqual(ca.representative(ms, toks)[0], 'Miss de Bourgh')

    def test_subjects_density_rewrites_only_clause_subjects_and_their_possessives(self):
        toks = tokens()
        mentions = [mention(TEXT, 'Mr. Bingley', 0), mention(TEXT, 'His', 0), mention(TEXT, 'he', 0), mention(TEXT, 'his', 0),
                    mention(TEXT, 'His sisters', 1), mention(TEXT, 'They', 1), mention(TEXT, 'them', 1), mention(TEXT, 'Their', 1),
                    mention(TEXT, 'Miss Bingley', 2), mention(TEXT, 'her', 2)]
        out = ca.apply(TEXT, ca.replacements(mentions, toks, 'replace', density='subjects'))
        self.assertEqual(out, 'Mr. Bingley came. Mr. Bingley’s sisters were fine women. Mr. Bingley’s sisters had a fortune of '
                              'twenty thousand pounds; Mr. Bingley admired them. Mr. Bingley’s sisters’ manners are not equal to his. '
                              'Miss Bingley kept her house; it stood near.')

    def test_apply_skips_edits_outside_the_unit_and_maps_document_offsets(self):
        self.assertEqual(ca.apply('cd', [(0, 1, 'A'), (2, 3, 'C'), (3, 5, 'XX')], offset=2), 'Cd')


if __name__ == '__main__':
    unittest.main()
