"""tools/power_source.sh: the power-source guard of the long measurements."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "tools/power_source.sh"
AC = "Now drawing from 'AC Power'\n -InternalBattery-0 (id=6815843)\t2%; charging; 3:15 remaining present: true\n"
BATTERY = "Now drawing from 'Battery Power'\n -InternalBattery-0 (id=6815843)\t1%; discharging; 0:04 remaining present: true\n"
DESKTOP = "Now drawing from 'AC Power'\n"


def run(text, **env_extra):
    env = {k: v for k, v in os.environ.items() if k != "SCRIBODO_SOAK_ON_BATTERY"}
    env.update(env_extra)
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "batt.txt"
        path.write_text(text)
        return subprocess.run([str(TOOL), "--from", str(path)], capture_output=True, text=True, env=env)


class PowerSourceTests(unittest.TestCase):
    def test_ac_power_passes_and_is_logged(self):
        r = run(AC)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "POWER source=AC Power charge=2%\n")

    def test_battery_refuses_with_the_reason(self):
        r = run(BATTERY)
        self.assertEqual(r.returncode, 3)
        self.assertEqual(r.stdout, "POWER source=Battery Power charge=1%\n")
        self.assertIn("on battery (1%)", r.stderr)
        self.assertIn("SCRIBODO_SOAK_ON_BATTERY=1", r.stderr)

    def test_battery_opt_in_passes_but_still_logs(self):
        r = run(BATTERY, SCRIBODO_SOAK_ON_BATTERY="1")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "POWER source=Battery Power charge=1%\n")

    def test_no_battery_is_not_a_refusal(self):
        r = run(DESKTOP)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "POWER source=AC Power charge=unknown\n")
        r = run("")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "POWER source=unknown charge=unknown\n")


if __name__ == "__main__":
    unittest.main()
