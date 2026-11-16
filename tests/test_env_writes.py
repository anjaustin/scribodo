"""The process environment is written only by app/startup_env.c, before the first thread (AUDIT-004 F12).

getenv/setenv across threads is unsynchronised: main() once set SCRIBODO_REMOTE_CONFIG and SCRIBODO_PROFILE after the
model supervisors, memory job and observer had started. This keeps every way of writing the environment in src/
(sources and headers) inside startup_env.c: any mention of setenv/unsetenv/putenv (a call, an alias, an address), and
any assignment through environ. And main() must call startup_env_prepare before any other function, so no phase
function, however it is split, can start a thread first (red team on remed/main)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ALLOWED = {SRC / "app" / "startup_env.c"}
WRITERS = re.compile(r"\b(setenv|unsetenv|putenv|_NSGetEnviron)\b")
ENVIRON_WRITE = re.compile(r"(\benviron\s*(\[[^\]]*\]\s*)?=(?!=))|(\*\s*\(?\s*environ\b[^;]*?[^=!<>]=(?!=))")
NOT_CALLS = {"if", "for", "while", "switch", "return", "sizeof", "_Static_assert", "defined"}


def code_text(text):
    """The text with comments and string/char literals blanked, line structure kept (enough for this tree's C)."""
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    text = re.sub(r"'(?:\\.|[^'\\\n])'", "''", text)
    return text


def env_writes(text):
    found = []
    for n, line in enumerate(code_text(text).splitlines(), 1):
        if WRITERS.search(line) or ENVIRON_WRITE.search(line):
            found.append((n, line.strip()))
    return found


def first_call_in_main(text):
    code = code_text(text)
    m = re.search(r"^int main\([^)]*\)\s*\{", code, flags=re.M)
    if not m:
        return None
    for call in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", code[m.end():]):
        if call.group(1) not in NOT_CALLS:
            return call.group(1)
    return None


class ScannerTests(unittest.TestCase):
    """The scanner itself catches what the red team showed slipping past the first version."""

    def test_catches_every_spelling(self):
        for snippet in ["static inline void f(void) { setenv(\"A\", \"b\", 1); }",
                        "#define SET_IT setenv",
                        "int (*w)(const char *, const char *, int) = &setenv;",
                        "void g(void) { putenv(buf); }",
                        "environ = fresh;",
                        "environ[0] = \"A=b\";",
                        "*environ = \"A=b\";",
                        "char ***e = _NSGetEnviron();"]:
            self.assertTrue(env_writes(snippet), snippet)

    def test_reads_and_comments_pass(self):
        for snippet in ["extern char **environ;",
                        "posix_spawn(&pid, bin, &fa, &attr, argv, environ);",
                        "if (environ == NULL) return;",
                        "/* setenv here would be wrong */",
                        "const char *msg = \"call setenv first\";"]:
            self.assertEqual(env_writes(snippet), [], snippet)

    def test_first_call(self):
        self.assertEqual(first_call_in_main("int main(void)\n{\n    static App a;\n    if (x) y();\n}"), "y")
        self.assertEqual(first_call_in_main("int main(int c, char **v)\n{\n    static S s;\n    (void)p(&s);\n}"), "p")


class EnvWriteTests(unittest.TestCase):
    def test_only_startup_env_writes_the_environment(self):
        found = []
        for path in sorted(SRC.rglob("*")):
            if path.suffix not in (".c", ".m", ".h") or path in ALLOWED:
                continue
            for n, line in env_writes(path.read_text(encoding="utf-8", errors="replace")):
                found.append(f"{path.relative_to(ROOT)}:{n}: {line}")
        self.assertEqual(found, [], "write the environment through app/startup_env.h, before any thread")

    def test_main_prepares_before_anything_else(self):
        first = first_call_in_main((SRC / "main.c").read_text(encoding="utf-8"))
        self.assertEqual(first, "startup_env_prepare",
                         "startup_env_prepare must be main()'s first call: nothing may start a thread before it")


if __name__ == "__main__":
    unittest.main()
