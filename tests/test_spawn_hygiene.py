#!/usr/bin/env python3
"""Every child the app starts gets only the descriptors it is handed (AUDIT-004, the register's "spawned tools inherit
the app's descriptors"). A static check, because a regression such as popen("ps …") in server.c passes every
behavioural test: no shell spawns in src/ (popen, system), no fork or exec*, and posix_spawn only in the two places
that set POSIX_SPAWN_CLOEXEC_DEFAULT (the shared helper and the model server's own spawn)."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPAWN_SITES = {"src/core/child_process.c", "src/llm/server.c"}


def code(path: Path) -> str:
    """The source without comments and string literals (a comment may name popen)."""
    text = path.read_text(errors="replace")
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)


class SpawnHygiene(unittest.TestCase):
    def sources(self) -> list[Path]:
        files = sorted(p for p in (ROOT / "src").rglob("*") if p.suffix in (".c", ".m", ".h"))
        self.assertTrue(files)
        return files

    def test_no_shell_or_fork(self) -> None:
        bad = []
        for p in self.sources():
            for m in re.finditer(r"\b(popen|system|fork|vfork|execl|execlp|execle|execv|execvp|execve)\s*\(", code(p)):
                bad.append(f"{p.relative_to(ROOT)}: {m.group(1)}()")
        self.assertEqual(bad, [], "spawn through src/core/child_process.h instead")

    def test_posix_spawn_only_with_cloexec_default(self) -> None:
        for p in self.sources():
            rel = str(p.relative_to(ROOT))
            text = code(p)
            if re.search(r"\bposix_spawnp?\s*\(", text):
                self.assertIn(rel, SPAWN_SITES, f"{rel} calls posix_spawn: use child_spawn()")
                self.assertIn("POSIX_SPAWN_CLOEXEC_DEFAULT", text, rel)

    def test_the_check_sees_a_popen(self) -> None:
        # The check itself: a popen in server.c's text (as before AUDIT-004) is found, a comment naming it is not.
        sample = 'int f(void) { /* popen( */ FILE *p = popen("ps", "r"); return 0; }'
        tmp = ROOT / "build" / "spawn_hygiene_sample.c"
        tmp.parent.mkdir(exist_ok=True)
        tmp.write_text(sample)
        try:
            self.assertEqual(len(re.findall(r"\bpopen\s*\(", code(tmp))), 1)
        finally:
            tmp.unlink()


if __name__ == "__main__":
    unittest.main()
