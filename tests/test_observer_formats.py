"""The observer's C-to-Python formats from the reader's side (src/llm/observer_formats.h, AUDIT-004 F7).

tests/fixtures/observer_formats/ is written by the C formatters (tests/test_observer_formats.c, byte for byte); this
test reads the same files with the tools that read real runs: tools/observer_judge_score.py load() for the judge log and
tools/observer_lines.py read() for the replay's finding lines. A format change that breaks a reader fails here; one
that breaks the writer fails there. No models."""
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/fixtures/observer_formats"
sys.path.insert(0, str(ROOT / "tools"))


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ojs = load("observer_judge_score")
lines = load("observer_lines")

JUDGE_MEMBERS = ["version", "kind", "doc", "message", "start", "len", "type", "path", "canon_id", "canon", "statement",
                 "replies", "conflict"]


class JudgeLogTests(unittest.TestCase):
    def setUp(self):
        self.calls = ojs.load(str(FIXTURES / "judge-log.jsonl"))

    def test_every_line_is_this_readers_version_with_the_members_in_order(self):
        self.assertEqual(len(self.calls), 3)
        for c in self.calls:
            self.assertEqual(c["version"], 1)
            self.assertIn(c["version"], ojs.JUDGE_LOG_VERSIONS)
            keys = [k for k in c if k not in ("attributes", "subject")]
            self.assertEqual(keys, JUDGE_MEMBERS)
            self.assertEqual(len(c["replies"]), 2)
            self.assertIn(c["kind"], ("doc", "chat"))
            self.assertIn(c["path"], ("statement", "sentence"))
            self.assertEqual(("attributes" in c, "subject" in c), (c["path"] == "sentence",) * 2)

    def test_fields_survive_utf8_quotes_newlines_and_nulls(self):
        doc, sentence, chat = self.calls
        self.assertEqual((doc["doc"], doc["start"], doc["len"], doc["conflict"]), ("Kapitel ø.md", 54, 21, True))
        self.assertEqual(json.loads(doc["replies"][1])["verdict"], "conflict")
        self.assertIsNone(sentence["replies"][1])
        self.assertEqual((chat["kind"], chat["message"]), ("chat", 3))
        self.assertEqual(chat["statement"], 'Sola\'s father is "Lorquas"\\Ptomel')
        self.assertEqual(json.loads(chat["replies"][0])["check"], "line one\nline two")

    def test_the_logged_co_signers_are_the_pythons_answers(self):
        """The C wrote attributes and subject with obs_attributes_differ and obs_same_subject; the scorer's a_any and
        same_subject must give the same answers on the same texts."""
        for c in self.calls:
            if c["path"] == "sentence":
                self.assertEqual(c["attributes"], ojs.a_any(c))
                self.assertEqual(c["subject"], ojs.same_subject(c))

    def test_the_scorer_labels_and_scores_the_log(self):
        faults = [{"id": "Z1", "file": "Kapitel ø.md", "char_offset": 60, "char_length": 5,
                   "canon": "Zoë's eyes are green."}]
        self.assertEqual(ojs.on_target(self.calls[0], faults)["id"], "Z1")
        res = ojs.score(self.calls, faults)
        self.assertEqual((res["calls"], res["reachable"], res["rows"][0]["found"]), (3, ["Z1"], ["Z1"]))

    def test_a_log_before_the_version_is_read_and_an_unknown_version_refused(self):
        with tempfile.TemporaryDirectory() as d:
            old = Path(d) / "old.jsonl"
            first = dict(self.calls[0]); del first["version"]
            old.write_text(json.dumps(first) + "\n", encoding="utf-8")
            self.assertEqual(ojs.load(str(old))[0]["doc"], "Kapitel ø.md")
            new = Path(d) / "new.jsonl"
            new.write_text(json.dumps(dict(self.calls[0], version=2)) + "\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                ojs.load(str(new))


class FindingLineTests(unittest.TestCase):
    def test_the_reader_returns_exactly_what_the_writer_formatted(self):
        rows, stats = lines.read(str(FIXTURES / "findings.txt"))
        want = json.loads((FIXTURES / "findings.json").read_text(encoding="utf-8"))
        self.assertEqual([{k: r[k] for k in lines.FIELDS} for r in rows], want)
        self.assertEqual((stats["records"], stats["joined"], stats["malformed"], stats["format"]), (3, 0, 0, 2))

    def test_the_marker_is_the_readers_format(self):
        err = (FIXTURES / "findings.err").read_text(encoding="utf-8")
        self.assertEqual(int(lines.FORMAT_MARKER.search(err).group(1)), lines.FORMAT)
        self.assertEqual(lines.format_of(str(FIXTURES / "findings.txt")), lines.FORMAT)

    def test_an_unknown_declared_format_is_refused_and_no_marker_is_a_run_before_it(self):
        with tempfile.TemporaryDirectory() as d:
            txt = Path(d) / "run.txt"
            shutil.copy(FIXTURES / "findings.txt", txt)
            self.assertIsNone(lines.read(str(txt))[1]["format"])             # no .err: a run before the marker
            (Path(d) / "run.err").write_text("# observer-findings format 3\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                lines.read(str(txt))


if __name__ == "__main__":
    unittest.main()
