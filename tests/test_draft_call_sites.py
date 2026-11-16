"""The load path settles the draft writer first (red team on remed/main, item 1).

doc_recover_strays renames every save-temp-named file beside a document, so no draft write may be in flight when it
runs. It runs only inside editor_load_file; this checks that open_path is the only caller of editor_load_file in src/
and that it calls draft_controller_before_load before it."""
from pathlib import Path
import re
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_env_writes import code_text  # noqa: E402  (comments and strings blanked)

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def function_body(code, name):
    m = re.search(r"^[\w \*]*\b%s\s*\([^;{]*\)\s*\{" % re.escape(name), code, flags=re.M)
    if not m:
        return None
    depth, i = 0, m.end() - 1
    for j in range(i, len(code)):
        depth += {"{": 1, "}": -1}.get(code[j], 0)
        if depth == 0:
            return code[i:j + 1]
    return None


class DraftCallSiteTests(unittest.TestCase):
    def test_recovery_runs_only_inside_editor_load_file(self):
        callers = []
        for path in sorted(SRC.rglob("*.[cm]")):
            code = code_text(path.read_text(encoding="utf-8", errors="replace"))
            if re.search(r"\bdoc_recover_strays\s*\(", code) and path.name not in ("doc_io.c", "editor.c"):
                callers.append(str(path.relative_to(ROOT)))
        self.assertEqual(callers, [])

    def test_open_path_is_the_only_load_and_settles_drafts_first(self):
        callers = []
        for path in sorted(SRC.rglob("*.[cm]")):
            if path.name == "editor.c":
                continue
            code = code_text(path.read_text(encoding="utf-8", errors="replace"))
            if re.search(r"\beditor_load_file\s*\(", code):
                callers.append(path.name)
        self.assertEqual(callers, ["main.c"], "a new load path must call draft_controller_before_load first")
        body = function_body(code_text((SRC / "main.c").read_text(encoding="utf-8")), "open_path")
        self.assertIsNotNone(body)
        settle = body.find("draft_controller_before_load(")
        load = body.find("editor_load_file(")
        self.assertGreaterEqual(settle, 0, "open_path must call draft_controller_before_load")
        self.assertLess(settle, load, "draft_controller_before_load must run before editor_load_file")
        self.assertEqual(len(re.findall(r"\beditor_load_file\s*\(", code_text((SRC / "main.c").read_text()))), 1)


if __name__ == "__main__":
    unittest.main()
