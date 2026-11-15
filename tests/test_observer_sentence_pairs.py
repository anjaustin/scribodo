"""tools/observer_sentence_pairs.py: source sentences paired with canon entries, and the reach grid."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("osp", ROOT / "tools/observer_sentence_pairs.py")
osp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(osp)


class SentenceTests(unittest.TestCase):
    def test_split_keeps_titles_initials_and_closing_quotes(self):
        text = "They dined at Mr. Philips’s. Then James K. Powell rode off.\n\n“My princess.” She smiled."
        self.assertEqual([s for _, s in osp.sentences(text)],
                         ["They dined at Mr. Philips’s.", "Then James K. Powell rode off.", "“My princess.”",
                          "She smiled."])
        for off, s in osp.sentences(text):
            self.assertEqual(text[off:off + len(s)], s)


class GridTests(unittest.TestCase):
    def test_reach_counts_the_faults_own_pair_under_the_rule(self):
        rows = [("a.md", 0, "s1"), ("a.md", 10, "s2")]
        canon = ["c1", "c2", "c3"]
        score = {(rows[0], "c1"): 4, (rows[0], "c2"): 2, (rows[0], "c3"): 5,
                 (rows[1], "c1"): 1, (rows[1], "c2"): 3, (rows[1], "c3"): 0}
        own = {"F1": (rows[0], "c1"), "F2": (rows[1], "c2")}
        res = {(m, k): (p, r) for m, k, p, r in osp.grid(canon, rows, score, own, mins=(3, 5), tops=(1, 2))}
        self.assertEqual(res[(3, 1)], (2, ["F2"]))          # s1's best is c3, not its fault's c1
        self.assertEqual(res[(3, 2)], (3, ["F1", "F2"]))
        self.assertEqual(res[(5, 2)], (1, []))


if __name__ == "__main__":
    unittest.main()
