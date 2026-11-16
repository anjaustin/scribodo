"""The process environment is written only by app/startup_env.c, before the first thread (AUDIT-004 F12).

getenv/setenv across threads is unsynchronised: main() once set SCRIBODO_REMOTE_CONFIG and SCRIBODO_PROFILE after the
model supervisors, memory job and observer had started. This keeps every write to the environment in src/ inside
startup_env.c, and main() calling startup_env_prepare before anything that can start a thread."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ALLOWED = {SRC / "app" / "startup_env.c"}
WRITE = re.compile(r"\b(setenv|unsetenv|putenv)\s*\(")


def code_lines(path):
    """Lines with comments and string literals blanked (enough for this tree's C)."""
    text = path.read_text(encoding="utf-8", errors="replace")
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    return text.splitlines()


class EnvWriteTests(unittest.TestCase):
    def test_only_startup_env_writes_the_environment(self):
        found = []
        for path in sorted(SRC.rglob("*.[cm]")):
            if path in ALLOWED:
                continue
            for n, line in enumerate(code_lines(path), 1):
                if WRITE.search(line):
                    found.append(f"{path.relative_to(ROOT)}:{n}: {line.strip()}")
        self.assertEqual(found, [], "write the environment through app/startup_env.h, before any thread")

    def test_main_prepares_before_the_first_worker(self):
        lines = code_lines(SRC / "main.c")
        start = next(i for i, l in enumerate(lines) if re.match(r"int main\(", l))
        body = lines[start:]
        prepare = next(i for i, l in enumerate(body) if "startup_env_prepare(" in l)
        # Everything main() calls that creates an object owning a thread, or starts one.
        starters = re.compile(r"\b(store_manager_create|\w+_job_init|\w+_job_create\w*|backup_job_init|"
                              r"service_supervisor_init|llm_server_start|observer_create|memory_job_create|"
                              r"pthread_create|InitWindow)\s*\(")
        first = next(i for i, l in enumerate(body) if starters.search(l))
        self.assertLess(prepare, first, "startup_env_prepare must run before main() starts anything")


if __name__ == "__main__":
    unittest.main()
