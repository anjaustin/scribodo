"""Object-level guard: only startup_env.o may link setenv, unsetenv, putenv or _NSGetEnviron (red team on remed/main).

The source scan (tests/test_env_writes.py) can be evaded by token pasting, a copy of environ, a split across lines and
so on; the compiled objects cannot hide a direct reference. Run by `make test` after the product's objects are built:

    python3 tests/test_env_symbols.py OBJECT...

(dlsym("setenv") still evades both; nothing in src/ calls dlsym.)"""
from pathlib import Path
import re
import subprocess
import sys
import unittest

FORBIDDEN = re.compile(r"^_?(setenv|unsetenv|putenv)(\$\S*)?$|^_?_NSGetEnviron$")
ALLOWED = {"startup_env.o"}
OBJECTS = []


def references(obj):
    out = subprocess.run(["nm", "-u", str(obj)], capture_output=True, text=True, check=True).stdout
    return sorted({s.strip() for s in out.split() if FORBIDDEN.match(s.strip())})


class EnvSymbolTests(unittest.TestCase):
    def test_objects_given(self):
        self.assertTrue(OBJECTS, "pass the product's objects")
        self.assertTrue(any(Path(o).name == "startup_env.o" for o in OBJECTS), "startup_env.o must be among them")

    def test_detector_sees_startup_env(self):
        """The check would see a reference: startup_env.o's own setenv is found."""
        env = [o for o in OBJECTS if Path(o).name == "startup_env.o"]
        self.assertTrue(env and "_setenv" in references(env[0]))

    def test_no_other_object_writes_the_environment(self):
        found = {}
        for obj in OBJECTS:
            if Path(obj).name in ALLOWED:
                continue
            refs = references(obj)
            if refs:
                found[str(obj)] = refs
        self.assertEqual(found, {}, "write the environment through app/startup_env.h, before any thread")

    def test_pattern(self):
        for s in ["_setenv", "_unsetenv", "_putenv", "__NSGetEnviron", "_setenv$UNIX2003"]:
            self.assertTrue(FORBIDDEN.match(s), s)
        for s in ["_getenv", "_environ", "_unsetenv_ok", "_my_setenv"]:
            self.assertFalse(FORBIDDEN.match(s), s)


if __name__ == "__main__":
    OBJECTS[:] = sys.argv[1:]
    unittest.main(argv=sys.argv[:1])
