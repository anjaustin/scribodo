#!/usr/bin/env python3
"""One SYSMEM line per sample: what the launch-time memory fit reads as available, and its parts.

usage: tools/sysmem_sample.py [--minutes N] [--interval S] [--from VMSTAT_FILE [--ps PS_FILE]]

avail_gb is sysmem_available() (src/llm/memfit.c) recomputed from vm_stat: total memory less wired pages, the
compressor's pages and anonymous pages beyond the purgeable ones. The other fields are the parts, plus the resident
sizes of the Docker virtual machine (com.apple.Virtualization.VirtualMachine), llama-server and the app, so a drop
in avail_gb can be read against who grew. --from reads one saved vm_stat text (and --ps a saved `ps -axo rss,comm`)
and prints one line, for tests. Read-only; run it beside a soak (GAP-003, 2026-11-13)."""
import argparse
import re
import subprocess
import sys
import time


def parse_vm_stat(text):
    page = int(re.search(r"page size of (\d+) bytes", text).group(1))
    counts = {m.group(1).strip(): int(m.group(2)) for m in re.finditer(r'^"?([^:"\n]+)"?:\s+(\d+)\.', text, re.M)}
    return page, counts


def available_bytes(total, page, counts):
    wired, comp = counts["Pages wired down"], counts["Pages occupied by compressor"]
    anon, purge = counts["Anonymous pages"], counts["Pages purgeable"]
    used = (wired + comp + max(anon - purge, 0)) * page
    return total - used if total > used else 0


def rss_by(ps_text, needle):
    kib = 0
    for line in ps_text.splitlines()[1:]:
        parts = line.split(None, 1)
        if len(parts) == 2 and needle(parts[1]):
            kib += int(parts[0])
    return kib * 1024


def line(total, vm_text, ps_text, now):
    page, counts = parse_vm_stat(vm_text)
    gb = lambda n: n / 1e9
    pages = lambda key: gb(counts[key] * page)
    vm = rss_by(ps_text, lambda c: "Virtualization.VirtualMachine" in c)
    llama = rss_by(ps_text, lambda c: "llama-server" in c)
    app = rss_by(ps_text, lambda c: c.strip().endswith("/scribodo") or c.strip() == "scribodo")
    return (f"SYSMEM at={now} avail_gb={gb(available_bytes(total, page, counts)):.2f} wired_gb={pages('Pages wired down'):.2f} "
            f"compressor_gb={pages('Pages occupied by compressor'):.2f} anon_gb={pages('Anonymous pages'):.2f} "
            f"purgeable_gb={pages('Pages purgeable'):.2f} free_gb={pages('Pages free'):.2f} inactive_gb={pages('Pages inactive'):.2f} "
            f"filebacked_gb={pages('File-backed pages'):.2f} vm_rss_gb={gb(vm):.2f} llama_rss_gb={gb(llama):.2f} app_rss_gb={gb(app):.2f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=60)
    ap.add_argument("--interval", type=float, default=60)
    ap.add_argument("--from", dest="vm_file")
    ap.add_argument("--ps", dest="ps_file")
    ap.add_argument("--total-bytes", type=int, help="with --from: the machine's memory (default: this machine's)")
    a = ap.parse_args()
    total = a.total_bytes or int(subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True, check=True).stdout)
    if a.vm_file:
        ps_text = open(a.ps_file).read() if a.ps_file else "RSS COMM\n"
        print(line(total, open(a.vm_file).read(), ps_text, "from-file"))
        return 0
    end = time.time() + a.minutes * 60
    while True:
        vm_text = subprocess.run(["vm_stat"], capture_output=True, text=True, check=True).stdout
        ps_text = subprocess.run(["ps", "-axo", "rss,comm"], capture_output=True, text=True, check=True).stdout
        print(line(total, vm_text, ps_text, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())), flush=True)
        if time.time() >= end:
            return 0
        time.sleep(a.interval)


if __name__ == "__main__":
    sys.exit(main())
