#!/usr/bin/env python3
"""Stream one owned app process and sample its RSS, with constant storage."""

import argparse
import os
import signal
import subprocess
import sys
import time


MAX_SAMPLES = 86400


class StopRequested(Exception):
    def __init__(self, signum):
        self.signum = signum


class RssStats:
    def __init__(self):
        self.attempts = 0
        self.count = 0
        self.errors = 0
        self.initial = self.final = self.peak = None
        self.first_at = self.last_at = None

    def sample(self, child):
        if self.attempts >= MAX_SAMPLES or child.poll() is not None:
            return
        self.attempts += 1
        try:
            result = subprocess.run(
                ["ps", "-o", "rss=", "-p", str(child.pid)],
                capture_output=True, text=True, timeout=2, check=False,
            )
            value = result.stdout.strip()
            if result.returncode != 0 or not value.isascii() or not value.isdecimal():
                if child.poll() is None:
                    self.errors += 1
                return
            rss = int(value)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            self.errors += 1
            return
        now = time.monotonic()
        if self.count == 0:
            self.initial = self.peak = rss
            self.first_at = now
        self.final = rss
        self.peak = max(self.peak, rss)
        self.last_at = now
        self.count += 1

    def report(self, pid, wall_seconds, exit_code):
        duration = self.last_at - self.first_at if self.count else 0.0
        age = time.monotonic() - self.last_at if self.count else 0.0
        def display(value):
            return "unavailable" if value is None else str(value)
        print(
            f"FRAME_SOAK_RSS pid={pid} initial_rss_kib={display(self.initial)} "
            f"final_rss_kib={display(self.final)} peak_rss_kib={display(self.peak)} "
            f"count={self.count} attempts={self.attempts} errors={self.errors} "
            f"sample_seconds={duration:.3f} final_sample_age_seconds={age:.3f} "
            f"process_wall_seconds={wall_seconds:.3f} sample_cap={MAX_SAMPLES} "
            f"sample_cap_reached={'yes' if self.attempts >= MAX_SAMPLES else 'no'} "
            f"exit_code={exit_code} "
            "(sampled app PID only; final=last live sample; "
            "fake backend/no LLM; excludes service RSS; not an SLA)",
            flush=True,
        )


def run(app):
    child = None
    stats = RssStats()
    started = time.monotonic()
    stopping = False
    exit_code = 1

    def stop(signum, _frame):
        nonlocal stopping
        if not stopping:
            stopping = True
            raise StopRequested(signum)

    previous = {sig: signal.signal(sig, stop) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        # Inherit stdout/stderr: frame metrics and script failures stream directly.
        child = subprocess.Popen([app], env={**os.environ, "SCRIBODO_LLM": "fake"})
        print(f"frame_soak: owned_app_pid={child.pid} rss_interval_seconds=1 backend=fake-no-LLM", flush=True)
        next_sample = time.monotonic()
        while child.poll() is None:
            now = time.monotonic()
            if now >= next_sample and stats.attempts < MAX_SAMPLES:
                stats.sample(child)
                # Do not burst samples to catch up after a slow ps invocation.
                next_sample = time.monotonic() + 1.0
            try:
                delay = next_sample - time.monotonic() if stats.attempts < MAX_SAMPLES else 0.1
                child.wait(timeout=max(0.01, min(0.1, delay)))
            except subprocess.TimeoutExpired:
                pass
        exit_code = child.returncode if child.returncode >= 0 else 128 - child.returncode
    except StopRequested as exc:
        exit_code = 128 + exc.signum
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"frame_soak: supervisor failed: {exc}", file=sys.stderr, flush=True)
        exit_code = 1
    finally:
        stopping = True
        try:
            if child is not None:
                # Popen retains ownership of this child. Never signal a process
                # group, service, or a PID obtained from a process-name search.
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
                stats.report(child.pid, time.monotonic() - started, exit_code)
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)
    return exit_code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", help="scripted app executable (launched as a direct child)")
    args = parser.parse_args()
    return run(args.app)


if __name__ == "__main__":
    sys.exit(main())
