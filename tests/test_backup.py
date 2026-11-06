"""File-only backup contract: no services, display, or model required."""
import os
import pathlib
import subprocess
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "tools" / "backup.sh"


class BackupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="scribodo-backup-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.project = self.root / "book"
        self.project.mkdir()
        (self.project / "chapter.md").write_text("Keep this chapter.\n")
        self.backup = self.root / "backup"

    def run_backup(self, *args, env=None):
        return subprocess.run([str(SCRIPT), *map(str, args)], capture_output=True, text=True, timeout=30, env=env)

    def test_regular_backup_verifies(self):
        result = self.run_backup("--no-db", self.project, self.backup)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(self.backup))
        verified = self.run_backup("verify", self.backup)
        self.assertEqual(verified.returncode, 0, verified.stderr)

    def test_links_fail_before_reporting_success(self):
        for target in ("chapter.md", "missing.md", str(self.root)):
            with self.subTest(target=target):
                link = self.project / "linked.md"
                link.symlink_to(target)
                try:
                    result = self.run_backup("--no-db", self.project, self.backup)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("link", result.stderr.lower())
                    self.assertFalse(self.backup.exists())
                finally:
                    link.unlink()

    def test_nested_checksum_file_is_included(self):
        (self.project / "SHA256SUMS").write_text("An author's file, not our checksum list.\n")
        result = self.run_backup("--no-db", self.project, self.backup)
        self.assertEqual(result.returncode, 0, result.stderr)
        verified = self.run_backup("verify", self.backup)
        self.assertEqual(verified.returncode, 0, verified.stderr)

    def test_existing_destination_is_untouched(self):
        self.backup.mkdir()
        sentinel = self.backup / "keep.md"
        sentinel.write_text("Keep me")
        result = self.run_backup("--no-db", self.project, self.backup)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_text(), "Keep me")

    def test_special_file_is_refused_without_blocking(self):
        os.mkfifo(self.project / "pipe")
        result = self.run_backup("--no-db", self.project, self.backup)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("special", result.stderr)
        self.assertFalse(self.backup.exists())

    def test_export_links_are_also_refused(self):
        exported = self.root / "exported"
        exported.mkdir()
        (exported / "link").symlink_to(self.project / "chapter.md")
        result = self.run_backup("--no-db", "--with", exported, self.project, self.backup)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("link", result.stderr)
        self.assertFalse(self.backup.exists())

    def test_destination_must_be_outside_the_source(self):
        nested = self.project / "backups" / "new"
        result = self.run_backup("--no-db", self.project, nested)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("outside", result.stderr)
        self.assertFalse(nested.exists())

    def test_dangling_destination_link_is_untouched(self):
        self.backup.symlink_to("missing")
        result = self.run_backup("--no-db", self.project, self.backup)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(self.backup.is_symlink())
        self.assertEqual(os.readlink(self.backup), "missing")

    def test_verification_failure_removes_only_the_new_backup(self):
        # Simulate a link appearing during the copy, after source preflight.
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        ditto = bin_dir / "ditto"
        ditto.write_text('#!/bin/bash\n/usr/bin/ditto "$@" || exit $?\nln -s chapter.md "$2/planted.md"\n')
        ditto.chmod(0o700)
        env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ["PATH"])
        result = self.run_backup("--no-db", self.project, self.backup, env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("links", result.stderr)
        self.assertFalse(self.backup.exists())
        self.assertEqual((self.project / "chapter.md").read_text(), "Keep this chapter.\n")

    def test_checksum_paths_preserve_unusual_names(self):
        for name in ("spaces and é.md", "line\nbreak.md", "back\\slash.md"):
            (self.project / name).write_text("Keep the filename too.\n")
        result = self.run_backup("--no-db", self.project, self.backup)
        self.assertEqual(result.returncode, 0, result.stderr)
        verified = self.run_backup("verify", self.backup)
        self.assertEqual(verified.returncode, 0, verified.stderr)

    def test_unreadable_extra_directory_fails_verification(self):
        result = self.run_backup("--no-db", self.project, self.backup)
        self.assertEqual(result.returncode, 0, result.stderr)
        hidden = self.backup / "unlisted"
        hidden.mkdir()
        (hidden / "extra.md").write_text("Not in the checksum list")
        hidden.chmod(0)
        try:
            verified = self.run_backup("verify", self.backup)
            self.assertNotEqual(verified.returncode, 0)
        finally:
            hidden.chmod(0o700)


if __name__ == "__main__":
    unittest.main()
