"""tools/observer_plant.py and tools/observer_score.py: the planted-fault recall measurement's tooling."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PLANT = ROOT / "tools/observer_plant.py"
SCORE = ROOT / "tools/observer_score.py"
SAMPLE = ROOT / "tools/observer_sample.py"
CH1 = "“Her eyes were blue,” he said. The road ran north.\n\nNothing else happened that day.\n"
CH2 = "The road ran north. Years passed.\n"
FAULTS = {"novel": "t", "faults": [
    {"id": "A1", "type": "appearance", "file": "ch1.md", "anchor": "The road ran north.",
     "insert": "Her eyes were brown.", "canon": "Her eyes are blue.", "fact": "ch1"},
    {"id": "A2", "type": "timeline", "file": "ch1.md", "anchor": "The road ran north.",
     "insert": "It was the year after.", "canon": "It was the same year.", "fact": "ch1"},
    {"id": "B1", "type": "place", "file": "ch2.md", "anchor": "Years passed.",
     "insert": "The road ran south.", "canon": "The road runs north.", "fact": "ch1"},
]}


def plant(faults, files):
    d = tempfile.mkdtemp()
    src, dst = Path(d) / "src", Path(d) / "dst"
    src.mkdir()
    for name, text in files.items():
        (src / name).write_text(text, encoding="utf-8")
    spec = Path(d) / "faults.json"
    spec.write_text(json.dumps(faults), encoding="utf-8")
    r = subprocess.run([str(PLANT), str(spec), str(src), str(dst)], capture_output=True, text=True)
    return r, dst


class PlantTests(unittest.TestCase):
    def test_inserts_after_the_anchor_and_records_byte_offsets(self):
        r, dst = plant(FAULTS, {"ch1.md": CH1, "ch2.md": CH2, "ch3.md": "Untouched.\n"})
        self.assertEqual(r.returncode, 0, r.stderr)
        ch1 = (dst / "ch1.md").read_text(encoding="utf-8")
        self.assertIn("The road ran north. Her eyes were brown. It was the year after.\n", ch1)
        self.assertTrue(ch1.startswith("“Her eyes were blue,”"))
        self.assertEqual((dst / "ch3.md").read_text(encoding="utf-8"), "Untouched.\n")
        applied = json.loads((dst / "faults-applied.json").read_text(encoding="utf-8"))["faults"]
        for a in applied:
            data = (dst / a["file"]).read_bytes()
            self.assertEqual(data[a["offset"]:a["offset"] + a["length"]].decode("utf-8"), a["insert"], a["id"])
        # the curly quotes are three bytes each: the byte offset is past the character offset
        a1 = next(a for a in applied if a["id"] == "A1")
        self.assertEqual(a1["offset"], CH1.encode("utf-8").index(b"The road ran north.") + len("The road ran north.") + 1)
        self.assertGreater(a1["offset"], CH1.index("The road ran north.") + len("The road ran north.") + 1)
        self.assertEqual(r.stdout, "--canon\tHer eyes are blue.\n--canon\tIt was the same year.\n--canon\tThe road runs north.\n")

    def test_an_ambiguous_or_missing_anchor_writes_nothing(self):
        bad = {"novel": "t", "faults": [dict(FAULTS["faults"][0], anchor="Nothing else"),
                                        dict(FAULTS["faults"][2], anchor="The road ran north.", file="ch1.md")]}
        r, dst = plant(bad, {"ch1.md": CH1 + "Nothing else.\n", "ch2.md": CH2})
        self.assertEqual(r.returncode, 2)
        self.assertIn("A1: anchor occurs 2 times", r.stderr)
        self.assertFalse(dst.exists())


class ScoreTests(unittest.TestCase):
    def test_recall_by_type_from_overlapping_conflict_spans(self):
        r, dst = plant(FAULTS, {"ch1.md": CH1, "ch2.md": CH2})
        self.assertEqual(r.returncode, 0, r.stderr)
        applied = {a["id"]: a for a in json.loads((dst / "faults-applied.json").read_text(encoding="utf-8"))["faults"]}
        a1, b1 = applied["A1"], applied["B1"]

        def line(t, doc, start, length, quote="q"):
            return f"{t}|s|r|o|new|content|doc|{doc}|{start}|{length}|{quote}|canon\n"
        findings = (line("conflict", "ch1.md", a1["offset"] + 4, 9)       # overlaps A1
                    + line("conflict", "ch1.md", a1["offset"] + 4, 9)     # a second on A1 counts for nothing else
                    + line("conflict", "ch2.md", 0, 5)                     # ch2 but before B1: not on a fault
                    + line("conflict", "ch2.md", b1["offset"] + b1["length"] - 1, 3)  # last byte of B1 overlaps
                    + line("entity", "ch1.md", a1["offset"], 5)           # not a conflict
                    + "# 5 findings\n")
        base = line("entity", "ch1.md", 0, 5) + line("entity", "ch1.md", 6, 5)
        d = Path(tempfile.mkdtemp())
        (d / "f.txt").write_text(findings, encoding="utf-8")
        (d / "b.txt").write_text(base, encoding="utf-8")
        r = subprocess.run([str(SCORE), str(dst / "faults-applied.json"), str(d / "f.txt"), str(d / "b.txt")],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = r.stdout.splitlines()
        self.assertEqual(out[0], "RECALL planted=3 found=2 appearance=1/1 timeline=0/1 place=1/1")
        self.assertEqual(out[1], "found: A1 B1")
        self.assertEqual(out[2], "missed: A2")
        self.assertIn("FINDINGS conflict=4 entity=1 total=5", r.stdout)
        self.assertIn("conflicts not on a planted fault: 2", r.stdout)
        self.assertIn("ADDED over the unplanted run: conflict=4 entity=-1", r.stdout)


class SampleTests(unittest.TestCase):
    def test_fixed_seed_sample_per_type_is_repeatable_and_in_file_order(self):
        def line(i, t):
            return f"{t}|subj{i}|r|o|new|content{i}|doc|ch1.md|{i * 10}|5|quote{i}|\n"
        text = "".join(line(i, "entity") for i in range(10)) + "".join(line(i, "event") for i in range(10, 12)) + "# 12 findings\n"
        d = Path(tempfile.mkdtemp())
        (d / "f.txt").write_text(text, encoding="utf-8")
        a = subprocess.run([str(SAMPLE), "7", "3", str(d / "f.txt")], capture_output=True, text=True)
        b = subprocess.run([str(SAMPLE), "7", "3", str(d / "f.txt")], capture_output=True, text=True)
        self.assertEqual(a.returncode, 0, a.stderr)
        self.assertEqual(a.stdout, b.stdout)
        rows = [l.split("\t") for l in a.stdout.splitlines()[1:]]
        self.assertEqual(a.stdout.splitlines()[0], "# seed=7 per_type=3 entity=10 event=2")
        self.assertEqual([r[0] for r in rows], ["entity"] * 3 + ["event"] * 2)
        idx = [int(r[1]) for r in rows[:3]]
        self.assertEqual(idx, sorted(idx))
        self.assertEqual(rows[0][2:5], ["ch1.md", str(idx[0] * 10), "5"])
        self.assertEqual(rows[3][5:], ["subj10", "content10", "quote10"])
        c = subprocess.run([str(SAMPLE), "8", "3", str(d / "f.txt")], capture_output=True, text=True)
        self.assertNotEqual(a.stdout, c.stdout)


if __name__ == "__main__":
    unittest.main()
