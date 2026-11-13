#!/usr/bin/env python3
"""Tabulate a series of manuscript-baseline runs (GAP-003 repeatability).

`tools/manuscript_series.sh` runs `tools/manuscript_baseline.sh` several times, each on a fresh disposable
database and project, and keeps each run's log as run-N.log in one directory. This tool reads those logs and
reports, per phase of tests/ui/store/16_manuscript_baseline.scribo, the spread of the phase's wall time, p99
interframe and worst frame across the runs (minimum, median, maximum), with the app's peak RSS, the service
containers' peaks and the process wall. A run whose log does not hold every phase, or whose script reported a
failure, is listed as incomplete and left out of the spread; the exit status is then 1.

The numbers are the baseline's (frame times include waits and vsync; not input latency, not an SLA); this tool
adds only how far repeated runs sit apart, which is what a budget has to be read against.

usage: python3 tools/manuscript_series.py DIR [--json OUT]
"""
import argparse
import json
import re
import statistics
import sys
from pathlib import Path

PHASES = ["import", "open chat", "page history", "chapter edit and save", "send", "enable memory", "recall"]
METRICS = re.compile(r"^FRAME_METRICS interframe_ms count=(\d+) p50=([\d.]+) p95=([\d.]+) p99=([\d.]+) max=([\d.]+) sample_seconds=([\d.]+)"
                     r"(?: over_50ms=(\d+))?")  # the count above the budget: absent in logs from before 2026-11-13
RSS = re.compile(r"^FRAME_SOAK_RSS .*?peak_rss_kib=(\d+|unavailable).*?process_wall_seconds=([\d.]+)")
SERVICES = re.compile(r"^FRAME_SOAK_SERVICES .*?(scribodo-\S+?:peak_kib=\d+:final_kib=\d+(?: scribodo-\S+?:peak_kib=\d+:final_kib=\d+)*)")
SERVICE = re.compile(r"(scribodo-[a-z]+)-\d+:peak_kib=(\d+):final_kib=(\d+)")
SUMMARY = re.compile(r"^\s*(\d+) passed, (\d+) failed\s*$")
WALL = re.compile(r"^manuscript_baseline: process_wall_seconds=(\d+)")


def parse_run(text):
    """One run's log: {'phases': [{count,p50,p95,p99,max,wall}...], 'peak_rss_kib', 'services': {name: peak_kib},
    'passed', 'failed', 'wall_seconds'}; missing pieces are None."""
    run = {"phases": [], "peak_rss_kib": None, "services": {}, "passed": None, "failed": None, "wall_seconds": None}
    for line in text.splitlines():
        m = METRICS.match(line)
        if m:
            count, p50, p95, p99, mx, wall, over = m.groups()
            run["phases"].append({"count": int(count), "p50": float(p50), "p95": float(p95), "p99": float(p99),
                                  "max": float(mx), "wall": float(wall), "over": None if over is None else int(over)})
            continue
        m = RSS.match(line)
        if m:
            run["peak_rss_kib"] = None if m.group(1) == "unavailable" else int(m.group(1))
            continue
        m = SERVICES.match(line)
        if m:
            for name, peak, _final in SERVICE.findall(m.group(1)):
                run["services"][name] = int(peak)
            continue
        m = SUMMARY.match(line)
        if m:
            run["passed"], run["failed"] = int(m.group(1)), int(m.group(2))
            continue
        m = WALL.match(line)
        if m:
            run["wall_seconds"] = int(m.group(1))
    return run


def incomplete(run):
    """Why a run is left out of the spread, or None."""
    if len(run["phases"]) != len(PHASES):
        return f"{len(run['phases'])} of {len(PHASES)} phases measured"
    if run["failed"] is None:
        return "no script summary"
    if run["failed"]:
        return f"{run['failed']} script checks failed"
    return None


def spread(values):
    return {"min": min(values), "median": statistics.median(values), "max": max(values), "n": len(values)}


def tabulate(runs):
    """The series: per-phase spreads over the complete runs, plus RSS, services and wall."""
    complete = [r for r in runs if incomplete(r) is None]
    out = {"runs": len(runs), "complete": len(complete), "phases": [], "peak_rss_kib": None, "services": {}, "wall_seconds": None}
    if not complete:
        return out
    for i, name in enumerate(PHASES):
        out["phases"].append({"phase": name,
                              "wall": spread([r["phases"][i]["wall"] for r in complete]),
                              "p99": spread([r["phases"][i]["p99"] for r in complete]),
                              "max": spread([r["phases"][i]["max"] for r in complete]),
                              "frames": spread([r["phases"][i]["count"] for r in complete]),
                              "over": spread([r["phases"][i]["over"] for r in complete]) if all(
                                  r["phases"][i]["over"] is not None for r in complete) else None})
    rss = [r["peak_rss_kib"] for r in complete if r["peak_rss_kib"] is not None]
    out["peak_rss_kib"] = spread(rss) if rss else None
    for name in sorted({n for r in complete for n in r["services"]}):
        vals = [r["services"][name] for r in complete if name in r["services"]]
        out["services"][name] = spread(vals)
    walls = [r["wall_seconds"] for r in complete if r["wall_seconds"] is not None]
    out["wall_seconds"] = spread(walls) if walls else None
    return out


def fmt(s, unit="", digits=2):
    return f"{s['min']:.{digits}f} / {s['median']:.{digits}f} / {s['max']:.{digits}f}{unit}"


def markdown(table, names):
    lines = [f"{table['complete']} complete of {table['runs']} runs ({', '.join(names)}); columns are min / median / max.", "",
             "| Phase | Wall s | p99 ms | Worst frame ms | Frames over 50 ms | Frames |", "|---|---:|---:|---:|---:|---:|"]
    for p in table["phases"]:
        over = fmt(p["over"], digits=0) if p["over"] else "not reported"
        lines.append(f"| {p['phase']} | {fmt(p['wall'])} | {fmt(p['p99'])} | {fmt(p['max'])} | {over} | {fmt(p['frames'], digits=0)} |")
    if table["peak_rss_kib"]:
        s = table["peak_rss_kib"]
        lines.append("")
        lines.append(f"App peak RSS MB: {s['min'] / 1024:.0f} / {s['median'] / 1024:.0f} / {s['max'] / 1024:.0f}")
    for name, s in table["services"].items():
        lines.append(f"{name} peak MB: {s['min'] / 1024:.0f} / {s['median'] / 1024:.0f} / {s['max'] / 1024:.0f}")
    if table["wall_seconds"]:
        lines.append(f"Process wall s: {fmt(table['wall_seconds'], digits=0)}")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dir", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    paths = sorted(args.dir.glob("run-*.log"), key=lambda p: int(re.search(r"run-(\d+)", p.name).group(1)))
    if not paths:
        print(f"no run-*.log in {args.dir}", file=sys.stderr)
        return 2
    runs = [parse_run(p.read_text(errors="replace")) for p in paths]
    status = 0
    for p, r in zip(paths, runs):
        why = incomplete(r)
        if why:
            print(f"incomplete: {p.name}: {why}")
            status = 1
    table = tabulate(runs)
    print(markdown(table, [p.name for p in paths]))
    if args.json:
        args.json.write_text(json.dumps({"table": table, "runs": [{"log": p.name, **r} for p, r in zip(paths, runs)]}, indent=1) + "\n")
    return status


if __name__ == "__main__":
    sys.exit(main())
