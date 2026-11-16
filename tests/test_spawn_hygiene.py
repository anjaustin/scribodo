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


# Names that only ever mean starting or loading code: banned as words, wherever they appear (a call, a macro alias
# `#define RUN popen`, a function pointer `= popen;`, a selector). NSTask is Foundation's process class, for the .m files.
BANNED = ["popen", "vfork", "forkpty", "daemon", "wordexp", "execl", "execlp", "execle", "execv", "execvp", "execvP",
          "execve", "execvpe", "execvP", "dlsym", "dlopen", "NSTask"]
# `system` and `fork` are also ordinary variable names here (a chat message's system part, a forked project), so they
# are banned as a call, a pointer taken (`= system;`, `&fork`), or a macro alias.
GENERIC = ["system", "fork"]


def findings(text: str) -> list[str]:
    found = [m.group(1) for m in re.finditer(r"\b(" + "|".join(sorted(set(BANNED))) + r")\b", text)]
    g = "|".join(GENERIC)
    for pattern in (r"\b(" + g + r")\s*\(",                              # a call
                    r"(?<![=!<>&|])=\s*(" + g + r")\s*[;,)]",            # a pointer assigned
                    r"(?<!&)&\s*(" + g + r")\b(?!\s*[&|])",              # a pointer taken
                    r"#\s*define\s+\w+(?:\([^)]*\))?\s+(" + g + r")\b"):  # a macro alias
        found += [m.group(1) for m in re.finditer(pattern, text)]
    return found


class SpawnHygiene(unittest.TestCase):
    def sources(self) -> list[Path]:
        files = sorted(p for p in (ROOT / "src").rglob("*") if p.suffix in (".c", ".m", ".h"))
        self.assertTrue(files)
        return files

    def test_no_shell_fork_or_loader(self) -> None:
        bad = [f"{p.relative_to(ROOT)}: {name}" for p in self.sources() for name in findings(code(p))]
        self.assertEqual(bad, [], "spawn through src/core/child_process.h instead")

    def test_posix_spawn_only_with_cloexec_default(self) -> None:
        for p in self.sources():
            rel = str(p.relative_to(ROOT))
            text = code(p)
            if re.search(r"\bposix_spawnp?\b", text):
                self.assertIn(rel, SPAWN_SITES, f"{rel} calls posix_spawn: use child_spawn()")
                self.assertIn("POSIX_SPAWN_CLOEXEC_DEFAULT", text, rel)

    def check_sample(self, sample: str, suffix: str = ".c") -> list[str]:
        tmp = ROOT / "build" / f"spawn_hygiene_sample{suffix}"
        tmp.parent.mkdir(exist_ok=True)
        tmp.write_text(sample)
        try:
            return findings(code(tmp))
        finally:
            tmp.unlink()

    def test_the_check_bites(self) -> None:
        # The verification's seven evasions, and the two generic names in their pointer and macro forms.
        caught = {
            '#define RUN popen\nFILE *f = RUN("ps", "r");': "popen",
            'FILE *(*open_cmd)(const char *, const char *) = popen;': "popen",
            'int (*run)(const char *) = dlsym(RTLD_DEFAULT, "system");': "dlsym",
            'wordexp_t w; wordexp("ps -p 1", &w, 0);': "wordexp",
            'int fd; pid_t p = forkpty(&fd, NULL, NULL, NULL);': "forkpty",
            'execvP("ps", "/bin", argv);': "execvP",
            '#define RUN_SHELL system\nint (*p)(const char *) = system;': "system",
            'pid_t (*f)(void) = &fork;': "fork",
        }
        for sample, name in caught.items():
            self.assertIn(name, self.check_sample(sample), sample)
        self.assertIn("NSTask", self.check_sample('NSTask *t = [[NSTask alloc] init];', ".m"))
        self.assertIn("popen", self.check_sample('int f(void) { /* popen( */ FILE *p = popen("ps", "r"); return 0; }'))
        # Ordinary variables named system and fork (as in answer_pipeline.c and store_import.c) are not spawns.
        for sample in ('cJSON *system = message("system", k), *turn = NULL; ok = add(messages, system) && system;',
                       'bool fork = strcmp(a, b) != 0; bool ok = fork ? x : y;',
                       'static char *complete(void *m, const char *system, const char *user);',
                       '/* system( in a comment */ const char *s = "popen(";'):
            self.assertEqual(self.check_sample(sample), [], sample)


if __name__ == "__main__":
    unittest.main()
