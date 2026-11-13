"""tools/manuscript_series.py: the series table is read from the runs' logs, and an incomplete run is named and left out."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import manuscript_series as ms  # noqa: E402

METRIC = ("FRAME_METRICS interframe_ms count={count} p50=17.250 p95=17.500 p99={p99} max={mx} sample_seconds={wall} "
          "(includes waits/vsync; not input latency, isolated CPU time, or an SLA)")


def run_log(walls, p99=18.0, mx=25.0, failed=0, rss=180000):
    lines = [METRIC.format(count=int(w * 60) or 1, p99=p99, mx=mx, wall=w) for w in walls]
    lines.append(f"FRAME_SOAK_RSS pid=1 initial_rss_kib=1600 final_rss_kib={rss} peak_rss_kib={rss} count=60 attempts=60 errors=0 "
                 "sample_seconds=60.0 final_sample_age_seconds=0.1 process_wall_seconds=61.0 sample_cap=86400 sample_cap_reached=no exit_code=0 ")
    lines.append("FRAME_SOAK_SERVICES samples=12 errors=0 scribodo-mariadb-1:peak_kib=526336:final_kib=520000 "
                 "scribodo-qdrant-1:peak_kib=474112:final_kib=470000 (docker stats memory usage of the local service containers; sampled about every 5 s)")
    lines.append(f"  {27 - failed} passed, {failed} failed")
    lines.append("manuscript_baseline: process_wall_seconds=61 words=100000 per_category=4 model=fake script=x manuscript=fixture")
    return "\n".join(lines) + "\n"


class ManuscriptSeriesTests(unittest.TestCase):
    def test_phases_rss_services_and_wall_are_read(self):
        run = ms.parse_run(run_log([0.9, 0.1, 0.5, 0.2, 0.9, 56.0, 0.25]))
        self.assertEqual(len(run["phases"]), 7)
        self.assertEqual(run["phases"][5]["wall"], 56.0)
        self.assertEqual(run["peak_rss_kib"], 180000)
        self.assertEqual(run["services"], {"scribodo-mariadb": 526336, "scribodo-qdrant": 474112})
        self.assertEqual((run["passed"], run["failed"], run["wall_seconds"]), (27, 0, 61))
        self.assertIsNone(ms.incomplete(run))

    def test_spread_is_min_median_max_over_complete_runs_only(self):
        runs = [ms.parse_run(run_log([1.0] * 7, mx=20.0)), ms.parse_run(run_log([3.0] * 7, mx=30.0)),
                ms.parse_run(run_log([2.0] * 7, mx=90.0, failed=1)), ms.parse_run(run_log([2.0] * 6))]
        self.assertEqual(ms.incomplete(runs[2]), "1 script checks failed")
        self.assertEqual(ms.incomplete(runs[3]), "6 of 7 phases measured")
        table = ms.tabulate(runs)
        self.assertEqual((table["runs"], table["complete"]), (4, 2))
        self.assertEqual(table["phases"][0]["phase"], "import")
        self.assertEqual(table["phases"][0]["wall"], {"min": 1.0, "median": 2.0, "max": 3.0, "n": 2})
        self.assertEqual(table["phases"][6]["max"]["max"], 30.0)  # the failed run's 90 ms frame is not in the spread
        text = ms.markdown(table, ["run-1.log", "run-2.log", "run-3.log", "run-4.log"])
        self.assertIn("2 complete of 4 runs", text)
        self.assertIn("| import | 1.00 / 2.00 / 3.00 |", text)
        self.assertIn("App peak RSS MB: 176 / 176 / 176", text)

    def test_no_complete_run_gives_an_empty_table(self):
        table = ms.tabulate([ms.parse_run("nothing here\n")])
        self.assertEqual(table["complete"], 0)
        self.assertEqual(table["phases"], [])


if __name__ == "__main__":
    unittest.main()
