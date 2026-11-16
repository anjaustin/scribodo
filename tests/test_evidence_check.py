"""tools/evidence_check.py: cited build/ paths against synthetic documents, manifests and archives (AUDIT-004 F1)."""
import io
import contextlib
import re
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import evidence_check as ec  # noqa: E402


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


class Tree:
    """A repository root, an archive folder and a build folder in one temporary directory."""

    def __init__(self, docs="", journal="", readme="", allow="", lost="", manifests=None, tarballs=None):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.root, self.evidence, self.build = base / "repo", base / "evidence", base / "repo/build"
        write(self.root / "docs/REPORT.md", docs)
        write(self.root / "journal/step.md", journal)
        write(self.root / "README.md", readme)
        write(self.root / ec.NOT_ARCHIVED, allow)
        write(self.root / ec.LOST, lost)
        (self.root / "tests/ui").mkdir(parents=True, exist_ok=True)
        self.evidence.mkdir()
        for name, lines in (manifests or {}).items():
            write(self.evidence / ("manifest-%s.txt" % name), "".join(l + "\n" for l in lines))
        for name, members in (tarballs or {}).items():
            src = base / "src"
            with tarfile.open(self.evidence / ("%s.tar.gz" % name), "w:gz") as t:
                for m in members:
                    f = src / m
                    write(f, "x")
                    t.add(f, arcname=m)

    def run(self, *extra):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = ec.main(["--root", str(self.root), "--evidence", str(self.evidence), *extra])
        return code, out.getvalue()

    def close(self):
        self.tmp.cleanup()


class NormalizeTests(unittest.TestCase):
    def test_trailing_punctuation_and_slashes(self):
        self.assertEqual(ec.normalize("build/rr1)"), ["build/rr1"])
        self.assertEqual(ec.normalize("build/a/b/,"), ["build/a/b"])
        self.assertEqual(ec.normalize("build/a.log."), ["build/a.log"])
        self.assertEqual(ec.normalize("build/x;"), ["build/x"])

    def test_braces_expand(self):
        self.assertEqual(ec.normalize("build/m-{a,b}.log"), ["build/m-a.log", "build/m-b.log"])
        self.assertEqual(ec.normalize("build/g/{gates,frames/gates}"), ["build/g/gates", "build/g/frames/gates"])
        self.assertEqual(ec.normalize("build/{a,b}/{c,d}"),
                         ["build/a/c", "build/a/d", "build/b/c", "build/b/d"])

    def test_numeric_range_expands(self):
        self.assertEqual(ec.normalize("build/ux1..3"), ["build/ux1", "build/ux2", "build/ux3"])

    def test_bare_build_is_not_a_citation(self):
        self.assertEqual(ec.normalize("build/"), [])

    def test_citation_boundaries(self):
        line = "rebuild/x and $(BUILD)/y and foo/build/z and `build/real/p`, (build/q)."
        found = [ec.normalize(m.group(0)) for m in ec.CITE.finditer(line)]
        self.assertEqual(found, [["build/real/p"], ["build/q"]])

    def test_patterns(self):
        rx = ec.pattern_regex("build/ui/<script>/saved.png")
        self.assertTrue(rx.match("build/ui/smoke/saved.png"))
        self.assertFalse(rx.match("build/ui/a/b/saved.png"))
        self.assertTrue(ec.pattern_regex("build/x-*.log").match("build/x-1.log"))
        self.assertTrue(ec.pattern_regex("build/runs/*").match("build/runs/r1/deep/file"))


class CheckTests(unittest.TestCase):
    def tree(self, **kw):
        t = Tree(**kw)
        self.addCleanup(t.close)
        return t

    def test_manifest_forms_cover_their_descendants(self):
        t = self.tree(docs="`build/mn-1/report.json` and `build/hangs/a.txt` and `build/mn-1`\n",
                      manifests={"2026-11-07": ["mn-1"], "2026-11-08b": ["build/hangs/", "build/hangs/a.txt"]})
        code, out = t.run()
        self.assertEqual(code, 0, out)
        self.assertIn("3 archived", out)

    def test_one_archived_file_does_not_cover_its_directory(self):
        # The red-team case: build/observer/old archived, build/observer cited, build/observer/new unarchived.
        t = self.tree(docs="`build/observer`\n", tarballs={"scribodo-evidence-a": ["build/observer/old/log.txt"]})
        write(t.build / "observer/old/log.txt", "archived")
        write(t.build / "observer/new/result.json", "{}")
        code, out = t.run()
        self.assertEqual(code, 1, out)
        self.assertIn("build/observer: 1 existing file(s)", out)
        self.assertIn("build/observer/new/result.json", out)
        write(t.build / "observer/new/result.json", "")  # still there: still a violation
        (t.build / "observer/new/result.json").unlink()
        self.assertEqual(t.run()[0], 0)

    def test_a_glob_citation_needs_every_match_archived(self):
        t = self.tree(docs="`build/g/gates-*.log`\n", tarballs={"scribodo-evidence-a": ["build/g/gates-test.log"]})
        write(t.build / "g/gates-test.log", "x")
        write(t.build / "g/gates-tsan.log", "x")
        code, out = t.run()
        self.assertEqual(code, 1)
        self.assertIn("build/g/gates-tsan.log", out)

    def test_relative_names_resolve_under_a_cited_directory(self):
        t = self.tree(journal="Runs in `build/gap003`.\nThe exit codes are in `gates-exit.txt`; `series-real-3/*` too.\n"
                              "`missing.txt` and `x.y` name nothing there.\n",
                      tarballs={"scribodo-evidence-a": ["build/gap003/soak.log"]})
        write(t.build / "gap003/soak.log", "x")
        write(t.build / "gap003/gates-exit.txt", "0")
        write(t.build / "gap003/series-real-3/run-1.log", "x")
        code, out = t.run("--list")
        self.assertEqual(code, 1)
        self.assertIn("journal/step.md:2: build/gap003/gates-exit.txt", out)
        self.assertIn("journal/step.md:2: build/gap003/series-real-3/*", out)
        self.assertNotIn("missing.txt", out)
        code, out = t.run("--unarchived")
        self.assertEqual(out.split(), ["build/gap003/gates-exit.txt", "build/gap003/series-real-3/run-1.log"])

    def test_the_build_folder_looked_at_is_reported(self):
        t = self.tree(docs="nothing\n")
        code, out = t.run()
        self.assertIn("files looked for under %s" % t.build, out)

    def test_a_file_stem_is_archived_through_its_files(self):
        t = self.tree(docs="`build/relation/rel4-before` `build/recover/gates2` `build/gap003/*/gates` `build/x/none`\n",
                      tarballs={"scribodo-evidence-a": ["build/relation/rel4-before.json", "build/relation/rel4-before.txt",
                                                        "build/recover/gates2-test.log", "build/gap003/reap/gates-tsan.log",
                                                        "build/x/none2.txt"]})
        code, out = t.run()
        self.assertEqual(code, 1, out)
        self.assertIn("build/x/none is in no archive", out)  # none2.txt is another stem
        self.assertIn("3 archived", out)
        # An existing stem file that is not archived fails the stem citation.
        write(t.build / "recover/gates2-tsan.log", "x")
        code, out = t.run()
        self.assertIn("build/recover/gates2: 1 existing file(s)", out)

    def test_tarball_listing_is_exact(self):
        # The manifest says the whole directory; the tarball holds one file. The tarball wins.
        t = self.tree(docs="`build/run/a.json` `build/run/b.json` `build/run`\n",
                      manifests={"scribodo-evidence-2026-11-12-p0": ["build/run"]},
                      tarballs={"scribodo-evidence-2026-11-12-p0": ["build/run/a.json"]})
        code, out = t.run()
        self.assertEqual(code, 1, out)
        self.assertIn("build/run/b.json is in no archive", out)
        self.assertNotIn("build/run/a.json is", out)
        self.assertIn("1 violation(s)", out)

    def test_top_level_tarball_names_get_build_prefix(self):
        t = self.tree(docs="`build/mn-2/x.log`\n", tarballs={"scribodo-evidence-2026-11-07": ["mn-2/x.log"]})
        self.assertEqual(t.run()[0], 0)

    def test_every_violation_is_listed_with_its_location(self):
        t = self.tree(docs="line\n`build/a` and `build/b`\n", journal="`build/c/{d,e}`\n")
        code, out = t.run()
        self.assertEqual(code, 1)
        for where in ["docs/REPORT.md:2: build/a", "docs/REPORT.md:2: build/b",
                      "journal/step.md:1: build/c/d", "journal/step.md:1: build/c/e"]:
            self.assertIn(where, out)
        self.assertIn("4 violation(s)", out)

    def test_readme_is_scanned(self):
        t = self.tree(readme="see `build/observer/redteam023/durability/`\n")
        code, out = t.run()
        self.assertEqual(code, 1)
        self.assertIn("README.md:1: build/observer/redteam023/durability is in no archive", out)

    def test_glob_and_placeholder_covered_by_any_member(self):
        t = self.tree(docs="`build/g/gates-*.log` `build/audit/<target>.log` `build/none-*.log`\n",
                      tarballs={"scribodo-evidence-x": ["build/g/gates-test.log", "build/audit/test.log"]})
        code, out = t.run()
        self.assertEqual(code, 1)
        self.assertIn("build/none-*.log is in no archive", out)
        self.assertIn("1 violation(s)", out)

    def test_not_evidence_needs_a_reason(self):
        t = self.tree(docs="`build/scribodo`\n", allow="build/scribodo\n")
        with self.assertRaises(SystemExit) as cm:
            t.run()
        self.assertIn("needs 2 tab-separated fields", str(cm.exception))

    def test_not_evidence_entries_cover_paths_dirs_and_globs(self):
        t = self.tree(docs="`build/scribodo` `build/venv-a/lib/x` `build/live-1/scribodo`\n",
                      allow="build/scribodo\tthe app\nbuild/venv-a/**\tenv\nbuild/live-*/scribodo\tfrozen binary\n")
        code, out = t.run()
        self.assertEqual(code, 0, out)
        self.assertIn("3 not-evidence", out)

    def test_entries_do_not_excuse_what_is_under_them(self):
        # The red-team's crafted cases: each passed as "not-evidence" under a plain directory entry.
        allow = ("build/ui\tthe gate root, named\nbuild/wt-ca908bd\ta worktree\nbuild/sanitize\ta build tree\n"
                 "build/test/DB\tprose\nbuild/answer-live-*/scribodo\tfrozen binary\n")
        for cited in ("build/ui/fit-on3/result.json", "build/wt-ca908bd/build/observer/run1/judge.log",
                      "build/sanitize/run/result.json", "build/test/DB/x.log", "build/answer-live-1/run/scribodo"):
            with self.subTest(cited=cited):
                t = self.tree(docs="`%s`\n" % cited, allow=allow)
                code, out = t.run()
                self.assertEqual(code, 1, out)
                self.assertIn(cited, out)
                self.assertNotIn("not-evidence)", out.splitlines()[-1])

    def test_a_star_stays_in_one_component(self):
        self.assertTrue(ec.entry_regex("build/memory-continuity-*").match("build/memory-continuity-ui.log"))
        self.assertFalse(ec.entry_regex("build/memory-continuity-*").match("build/memory-continuity-x/run/a.log"))
        self.assertTrue(ec.entry_regex("build/x/**").match("build/x"))
        self.assertTrue(ec.entry_regex("build/x/**").match("build/x/a/b"))
        self.assertFalse(ec.entry_regex("build/x/**").match("build/xy"))
        self.assertTrue(ec.entry_regex("build/ui/<script>/work").match("build/ui/smoke/work"))

    def test_lost_entries_are_exact_too(self):
        t = self.tree(docs="`build/gone/run/result.json` `build/old-a/run.log`\n",
                      lost="build/gone/run\tdocs/REPORT.md\tlost\nbuild/old-*\tdocs/REPORT.md\tlost\n")
        code, out = t.run()
        self.assertEqual(code, 1)
        self.assertIn("build/gone/run/result.json is in no archive", out)
        self.assertIn("build/old-a/run.log is in no archive", out)

    def test_lost_entries_are_accepted_until_the_path_reappears(self):
        t = self.tree(docs="`build/gone/run` `build/old-x.log`\n",
                      lost="build/gone/run\tdocs/REPORT.md\tlost\nbuild/old-*.log\tdocs/REPORT.md\tlost\n")
        self.assertEqual(t.run()[0], 0)
        write(t.build / "gone/run/result.json", "{}")
        code, out = t.run()
        self.assertEqual(code, 1)
        self.assertIn("build/gone/run is recorded as lost but exists", out)

    def test_lost_needs_the_citing_document(self):
        t = self.tree(docs="`build/gone`\n", lost="build/gone\n")
        with self.assertRaises(SystemExit):
            t.run()

    def test_gate_owned_paths_need_an_archive_not_an_excuse(self):
        owned = ec.gate_owned_dirs(ROOT)[0]
        t = self.tree(docs="`%s/fit-on` and `%s`\n" % (owned, owned),
                      allow="%s\tthe gate's own directory, named\n%s/*\tglob excuse\n" % (owned, owned))
        code, out = t.run()
        self.assertEqual(code, 1)
        self.assertIn("%s/fit-on is under %s, which a gate deletes" % (owned, owned), out)
        self.assertIn("1 violation(s)", out)  # the directory itself, named exactly, is not a violation
        # Archived, the same citation passes.
        t2 = self.tree(docs="`%s/fit-on`\n" % owned, tarballs={"scribodo-evidence-y": ["%s/fit-on/log" % owned]})
        self.assertEqual(t2.run()[0], 0)

    def test_stale_entries_warn_without_failing(self):
        t = self.tree(docs="nothing cited\n", allow="build/x\tunused\n")
        code, out = t.run()
        self.assertEqual(code, 0)
        self.assertIn("warning: no document cites", out)

    def test_absent_archive_folder_skips_with_a_message(self):
        out = io.StringIO()
        with tempfile.TemporaryDirectory() as d, contextlib.redirect_stdout(out):
            code = ec.main(["--root", str(ROOT), "--evidence", str(Path(d) / "missing")])
        self.assertEqual(code, 0)
        self.assertIn("evidence_check: skipped: no archive folder", out.getvalue())

    def test_unarchived_lists_the_next_archive_pass(self):
        t = self.tree(docs="`build/run` `build/tool` `build/old`\n", allow="build/tool\ta binary\n",
                      tarballs={"scribodo-evidence-z": ["build/run/a.txt"]})
        write(t.build / "run/a.txt", "archived")
        write(t.build / "run/b.txt", "new")
        write(t.build / "run/c.o", "object")
        write(t.build / "run/x.dSYM/Contents/Info.plist", "symbols")
        (t.build / "run/bin").write_bytes(b"\xcf\xfa\xed\xfe" + b"\0" * 16)
        write(t.build / "tool/inner.txt", "not evidence")
        write(t.build / "old/z.txt", "unarchived")
        code, out = t.run("--unarchived")
        self.assertEqual(code, 0)
        self.assertEqual(out.split(), ["build/old/z.txt", "build/run/b.txt"])


class GateOwnedDirsTests(unittest.TestCase):
    """Every directory a gate deletes under build/ is one evidence_check treats as gate-owned."""

    def deleted_paths(self):
        text = (ROOT / "Makefile").read_text()
        paths = []
        for target, recipe in re.findall(r"^([A-Za-z_-]+):[^\n]*\n((?:\t[^\n]*\n)+)", text, re.M):
            if target == "distclean":
                continue
            for args in re.findall(r"rm -rf ([^;&|\n]+)", recipe):
                for a in args.split():
                    paths.append(a.replace("$(BUILD)", "build"))
        for script in ("backuptest.sh", "offlinetest.sh", "remotetest.sh"):
            text = (ROOT / "tools" / script).read_text()
            for m in re.findall(r"\$PWD/(build/ui/[a-z]+)", text):
                paths.append(m)
        text = (ROOT / "tools/cstest.sh").read_text()
        paths += re.findall(r"=(build/[A-Za-z.]+)", text)
        clean = subprocess.run(["make", "-n", "-C", str(ROOT), "clean"], capture_output=True, text=True)
        self.assertEqual(clean.returncode, 0, clean.stderr)
        removed = clean.stdout.split()
        self.assertEqual(removed[:2], ["rm", "-rf"])
        self.clean_removes = removed[2:]
        return paths + [p for p in removed[2:] if not re.match(r"build/(scribodo|test_[a-z_]+)$", p)]

    def test_deletions_are_owned(self):
        owned = ec.gate_owned_dirs(ROOT)
        files = {"$(APP)", "$(TESTS)"}
        # The loops run UI_SCRIPTS and REAL_UI_SCRIPTS: every script but case_sensitive (cstest).
        stems = [s.stem for s in (ROOT / "tests/ui").glob("*.scribo") if s.stem != "case_sensitive"]
        checked = 0
        for p in self.deleted_paths():
            if p in files or not p.startswith("build/"):
                continue
            # build/ui/$$name: the recipe's loop over the UI scripts.
            for q in ([p.replace("$$name", s) for s in stems] if "$$name" in p else [p]):
                q = q.rstrip("/")
                checked += 1
                self.assertTrue(any(q == d or q.startswith(d + "/") for d in owned),
                                "%s is deleted by a gate but not in gate_owned_dirs" % q)
        self.assertGreater(checked, 3)
        self.assertNotIn("build/ui", self.clean_removes)  # no gate deletes the whole of build/ui
        # Every per-script directory the gates own, make clean also removes.
        for d in owned:
            if d.startswith("build/ui/"):
                self.assertIn(d, self.clean_removes)

if __name__ == "__main__":
    unittest.main(verbosity=2)
