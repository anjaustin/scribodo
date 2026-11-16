#!/usr/bin/env python3
"""The services' defaults are written once (AUDIT-004 F6): tools/services.defaults for the scripts and Compose,
src/app/service_defaults.h for the C side. Fails if a copy disagrees or a literal copy reappears."""

from __future__ import annotations

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = ROOT / "tools/services.defaults"
HEADER = ROOT / "src/app/service_defaults.h"

# Where the values used to be written, and must not be again.
PRODUCT_FILES = [
    "tools/services_lib.sh", "tools/services.sh", "tools/backup.sh", "tools/restore.sh", "docker/compose.yml",
    "src/app/db.c", "src/app/db.h", "src/llm/semantic.c", "src/llm/remote.c", "tests/db_admin.c",
]


def read_defaults() -> dict[str, str]:
    values = {}
    for line in DEFAULTS.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.fullmatch(r"([A-Z_]+)=(.*)", line) or re.fullmatch(r': "\$\{([A-Z_]+):=([^}]*)\}"', line)
        assert m, f"tools/services.defaults: not a plain assignment or overridable default: {line}"
        words = shlex.split(m.group(2))
        assert len(words) == 1, f"tools/services.defaults: {m.group(1)} is not one word or one quoted string"
        values[m.group(1)] = words[0]
    return values


def clean_env(home: str) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("SCRIBODO_")}
    env["HOME"] = home
    return env


class ServiceDefaults(unittest.TestCase):
    def setUp(self) -> None:
        self.d = read_defaults()

    def test_header_matches(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "probe.c", Path(tmp) / "probe"
            src.write_text('#include <stdio.h>\n#include "app/service_defaults.h"\n'
                           'int main(void) { printf("%d\\n%d\\n%s\\n", SCRIBODO_DEFAULT_DB_PORT, '
                           'SCRIBODO_DEFAULT_QDRANT_PORT, SCRIBODO_DEFAULT_SECRETS_DIR); return 0; }\n')
            subprocess.run(["cc", "-I", str(ROOT / "src"), str(src), "-o", str(exe)], check=True)
            db, q, secrets = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.splitlines()
        self.assertEqual(db, self.d["SCRIBODO_DEFAULT_DB_PORT"])
        self.assertEqual(q, self.d["SCRIBODO_DEFAULT_QDRANT_PORT"])
        self.assertEqual(secrets, self.d["SCRIBODO_DEFAULT_SECRETS_DIR"])

    def test_scripts_read_the_file(self) -> None:
        # The library, sourced with nothing set, under a throwaway HOME (no store is read or touched).
        with tempfile.TemporaryDirectory() as home:
            out = subprocess.run(
                ["bash", "-c", '. tools/services_lib.sh; printf "%s\\n" "$SCRIBODO_DB_PORT" "$SCRIBODO_QDRANT_PORT" '
                 '"$SCRIBODO_SECRETS" "$DEFAULT_SECRETS"; bash -c \'printf "%s\\n" "$SCRIBODO_MARIADB_SERVER_FLAGS"\''],
                cwd=ROOT, env=clean_env(home), check=True, capture_output=True, text=True).stdout.splitlines()
            secrets = f"{home}/{self.d['SCRIBODO_DEFAULT_SECRETS_DIR']}"
        self.assertEqual(out, [self.d["SCRIBODO_DEFAULT_DB_PORT"], self.d["SCRIBODO_DEFAULT_QDRANT_PORT"], secrets,
                               secrets, self.d["SCRIBODO_MARIADB_SERVER_FLAGS"]])

    def test_native_server_flags(self) -> None:
        text = (ROOT / "tools/services.sh").read_text()
        body = re.search(r"^mariadbd_args\(\) \{\n(.*?)^\}", text, re.S | re.M)
        self.assertIsNotNone(body)
        self.assertIn('<<< "$SCRIBODO_MARIADB_SERVER_FLAGS"', body.group(1))
        self.assertIn('"${flags[@]}"', body.group(1))

    def test_literal_check_bites(self) -> None:
        # The red team's example: the pre-AUDIT-004 services_lib.sh default is caught.
        old = 'export SCRIBODO_QDRANT_PORT="${SCRIBODO_QDRANT_PORT:-6333}"'
        self.assertRegex(old, r"\b" + self.d["SCRIBODO_DEFAULT_QDRANT_PORT"] + r"\b")
        self.assertRegex(old, r":-" + self.d["SCRIBODO_DEFAULT_QDRANT_PORT"] + r"\b")

    def test_compose_has_no_copy(self) -> None:
        text = (ROOT / "docker/compose.yml").read_text()
        mariadb = text.split("\n  qdrant:")[0]
        self.assertRegex(mariadb, r"\n    command: \$\{SCRIBODO_MARIADB_SERVER_FLAGS:\?[^}]*\}\n")
        self.assertIn('"127.0.0.1:${SCRIBODO_DB_PORT:?', text)
        self.assertIn('"127.0.0.1:${SCRIBODO_QDRANT_PORT:?', text)
        self.assertNotRegex(text, r"\$\{SCRIBODO_(DB|QDRANT)_PORT:-")

    def test_no_literal_copies(self) -> None:
        flags = self.d["SCRIBODO_MARIADB_SERVER_FLAGS"].split()
        literals = [self.d["SCRIBODO_DEFAULT_DB_PORT"], self.d["SCRIBODO_DEFAULT_SECRETS_DIR"],
                    "Application Support/Scribodo/services", *flags]
        for rel in PRODUCT_FILES:
            text = (ROOT / rel).read_text()
            for literal in literals:
                self.assertNotIn(literal, text, f"{rel} writes {literal!r}; it belongs in tools/services.defaults")
            q = self.d["SCRIBODO_DEFAULT_QDRANT_PORT"]
            if rel.endswith((".c", ".h", ".sh")):
                # Qdrant's port in C or shell, a literal or a shell default such as ${SCRIBODO_QDRANT_PORT:-6333}
                self.assertNotRegex(text, r"\b" + q + r"\b", rel)
            for port in (q, self.d["SCRIBODO_DEFAULT_DB_PORT"]):
                # a default in any shell or Compose expansion (Compose keeps 6333 only as the container's own port)
                self.assertNotRegex(text, r":-" + port + r"\b", rel)

    def test_mariadb_pin_is_one_release(self) -> None:
        compose = (ROOT / "docker/compose.yml").read_text()
        tag = re.search(r"image: mariadb:([^@\s]+)@sha256:[0-9a-f]{64}", compose)
        self.assertIsNotNone(tag)
        self.assertEqual(tag.group(1), self.d["SCRIBODO_MARIADB_VERSION"])

    def test_variables_documented(self) -> None:
        # Every variable the services scripts and Compose read is in services.sh's header block (AUDIT-004 F6).
        header = (ROOT / "tools/services.sh").read_text().split("\nset -euo pipefail", 1)[0]
        documented = set(re.findall(r"^#   (SCRIBODO_[A-Z_]+)", header, re.M)) - {"SCRIBODO_DEFAULT_SECRETS_DIR"}
        used = set()
        for rel in ("tools/services.sh", "tools/services_lib.sh", "docker/compose.yml"):
            used |= set(re.findall(r"SCRIBODO_[A-Z_]+", (ROOT / rel).read_text()))
        used -= {k for k in self.d if k.startswith("SCRIBODO_DEFAULT_")}
        self.assertEqual(sorted(used - documented), [], "used by the scripts but not in services.sh's header")
        self.assertEqual(sorted(documented - used), [], "documented in services.sh's header but never read")


if __name__ == "__main__":
    unittest.main()
