"""tools/observer_judge_score.py: judge calls labelled against planted faults, and the rules scored."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/observer_judge_score.py"
spec = importlib.util.spec_from_file_location("ojs", TOOL)
ojs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ojs)

FAULTS = [{"id": "M7", "type": "appearance", "file": "ch13.md", "char_offset": 100, "char_length": 60,
           "canon": "Dejah Thoris's hair is coal black.", "insert": "Her golden hair fell loose about her shoulders."},
          {"id": "M8", "type": "kinship", "file": "ch16.md", "char_offset": 500, "char_length": 70,
           "canon": "Sola is the daughter of Tars Tarkas.", "insert": "Sola was the daughter of Lorquas Ptomel."}]


def call(doc, start, length, canon, statement, conflict, kind="doc"):
    return {"kind": kind, "doc": doc, "start": start, "len": length, "canon": canon, "statement": statement,
            "conflict": conflict, "replies": [None, None]}


class LabelTests(unittest.TestCase):
    def test_on_target_needs_the_span_the_file_and_the_faults_own_canon(self):
        self.assertEqual(ojs.on_target(call("ch13.md", 120, 10, FAULTS[0]["canon"], "x", True), FAULTS)["id"], "M7")
        self.assertIsNone(ojs.on_target(call("ch13.md", 10, 10, FAULTS[0]["canon"], "x", True), FAULTS))       # before it
        self.assertIsNone(ojs.on_target(call("ch14.md", 120, 10, FAULTS[0]["canon"], "x", True), FAULTS))      # other file
        self.assertIsNone(ojs.on_target(call("ch13.md", 120, 10, FAULTS[1]["canon"], "x", True), FAULTS))      # other canon
        self.assertIsNone(ojs.on_target(call("ch13.md", 120, 10, FAULTS[0]["canon"], "x", True, "chat"), FAULTS))
        self.assertEqual(ojs.on_target(call("ch13.md", 159, 5, FAULTS[0]["canon"], "x", True), FAULTS)["id"], "M7")  # last char


class RuleTests(unittest.TestCase):
    def test_attribute_detectors(self):
        self.assertTrue(ojs.a_color({"canon": "Her hair is coal black.", "statement": "Her golden hair fell."}))
        self.assertFalse(ojs.a_color({"canon": "Her hair is black.", "statement": "She wore silks."}))      # one side only
        self.assertTrue(ojs.a_kin({"canon": "Sola is the daughter of Tars.", "statement": "Sola was the sister of Lorquas."}))
        self.assertFalse(ojs.a_kin({"canon": "Sola is the daughter of Tars.", "statement": "Sola, the daughter of Tars, ran."}))
        self.assertTrue(ojs.a_num({"canon": "thirty days after", "statement": "only three days after"}))
        self.assertTrue(ojs.a_place({"canon": "the Princess of Helium.", "statement": "the Princess of Zodanga."}))
        self.assertFalse(ojs.a_place({"canon": "the Princess of Helium.", "statement": "she smiled at the Princess."}))
        self.assertTrue(ojs.a_subject({"canon": "Dejah Thoris is the Princess of Helium.", "statement": "Dejah Thoris wore silks."}))
        self.assertFalse(ojs.a_subject({"canon": "Mr. Darcy's estate.", "statement": "Mr. Wickham's regiment."}))  # a title is no name

    def test_subject_and_slot(self):
        self.assertEqual(ojs.subjects("Kantos Kan \u2014 A former padwar in the navy of Zodanga."), {"Kantos", "Kan"})
        self.assertEqual(ojs.subjects("Captain James K. Powell is of Richmond."), {"James", "Powell"})
        self.assertIn("Sola", ojs.subjects("Lorquas Ptomel \u2014 Sola's father."))
        self.assertTrue(ojs.same_subject({"canon": "Captain James K. Powell is of Richmond.", "statement": "Powell \u2014 born in Boston."}))
        self.assertFalse(ojs.same_subject({"canon": "Dejah Thoris is the Princess of Helium.",
                                           "statement": "Sab Than \u2014 a man whose death Dejah Thoris sealed."}))
        self.assertTrue(ojs.same_slot({"canon": "Kantos Kan is a padwar in the navy of Helium.", "statement": "Kantos Kan, padwar of Zodanga."}))
        self.assertFalse(ojs.same_slot({"canon": "Kantos Kan's vessel reached Helium.", "statement": "Kantos Kan says he waited."}))
        self.assertTrue(ojs.same_slot({"canon": "Sola is the daughter of Tars Tarkas.", "statement": "Sola sang."}))  # no slot word

    def test_scored_table(self):
        calls = [call("ch13.md", 120, 10, FAULTS[0]["canon"], "Dejah Thoris: her golden hair fell loose.", True),
                 call("ch01.md", 5, 10, FAULTS[0]["canon"], "Dejah Thoris wore silks.", True),          # an addition
                 call("ch16.md", 510, 10, FAULTS[1]["canon"], "Sola was the daughter of Lorquas Ptomel.", False),
                 call("ch02.md", 5, 10, FAULTS[1]["canon"], "The road ran north.", False)]
        res = ojs.score(calls, FAULTS)
        self.assertEqual(res["reachable"], ["M7", "M8"])
        rows = {r["rule"]: r for r in res["rows"]}
        base = rows["base: the judge's two votes"]
        self.assertEqual((base["kept"], base["found"], base["false"]), (2, ["M7"], 1))
        attr = rows["judge and any differing attribute"]
        self.assertEqual((attr["kept"], attr["found"], attr["false"]), (1, ["M7"], 0))
        pre = rows["pre: a shared name and any differing attribute"]
        self.assertEqual((pre["found"], pre["false"], pre["calls_needed"]), (["M7", "M8"], 0, 2))

    def test_command_line(self):
        d = Path(tempfile.mkdtemp())
        (d / "j.jsonl").write_text(json.dumps(call("ch13.md", 120, 10, FAULTS[0]["canon"], "golden hair", True)) + "\n")
        (d / "f.json").write_text(json.dumps({"faults": FAULTS}))
        r = subprocess.run([str(TOOL), str(d / "j.jsonl"), str(d / "f.json")], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("JUDGE calls=1 faults=2 reachable=1 (M7)", r.stdout)


if __name__ == "__main__":
    unittest.main()
