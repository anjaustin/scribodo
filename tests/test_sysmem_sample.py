"""tools/sysmem_sample.py: the memory fit's available figure recomputed from a saved vm_stat."""
from pathlib import Path
import subprocess
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "tools/sysmem_sample.py"
# 16384-byte pages; the counts below make the arithmetic exact in GB.
VM_STAT = """Mach Virtual Memory Statistics: (page size of 16384 bytes)
Pages free:                               61035.
Pages active:                            200000.
Pages inactive:                          305175.
Pages speculative:                         1000.
Pages throttled:                              0.
Pages wired down:                        122070.
Pages purgeable:                           6103.
"Translation faults":                  99999999.
Pages copy-on-write:                      12345.
Pages zero filled:                        12345.
Pages reactivated:                        12345.
Pages purged:                             12345.
File-backed pages:                       183105.
Anonymous pages:                         305175.
Pages stored in compressor:              500000.
Pages occupied by compressor:            122070.
Decompressions:                               1.
Compressions:                                 1.
Pageins:                                      1.
Pageouts:                                     1.
Swapins:                                      0.
Swapouts:                                     0.
"""
PS = """  RSS COMM
1550000 /System/Library/Frameworks/Virtualization.framework/Versions/A/XPCServices/com.apple.Virtualization.VirtualMachine.xpc/Contents/MacOS/com.apple.Virtualization.VirtualMachine
4100000 /Users/x/Projects/scribodo/build/llama/bin/llama-server
 160000 /Users/x/Projects/scribodo/build/scribodo
 300000 /Applications/Other.app/Contents/MacOS/Other
"""


def run(ps=None):
    with tempfile.TemporaryDirectory() as d:
        vm = Path(d) / "vm_stat.txt"
        vm.write_text(VM_STAT)
        args = [str(TOOL), "--from", str(vm), "--total-bytes", str(16 * 1024**3)]
        if ps is not None:
            p = Path(d) / "ps.txt"
            p.write_text(ps)
            args += ["--ps", str(p)]
        return subprocess.run(args, capture_output=True, text=True)


class SysmemSampleTests(unittest.TestCase):
    def fields(self, out):
        return dict(kv.split("=", 1) for kv in out.split()[1:])

    def test_available_is_total_less_wired_compressor_and_unpurgeable_anonymous(self):
        r = run()
        self.assertEqual(r.returncode, 0, r.stderr)
        f = self.fields(r.stdout)
        # 16 GiB = 17.18 GB; used = (122070 + 122070 + (305175 - 6103)) * 16384 = 8.90 GB
        self.assertEqual(f["avail_gb"], "8.28")
        self.assertEqual(f["wired_gb"], "2.00")
        self.assertEqual(f["compressor_gb"], "2.00")
        self.assertEqual(f["anon_gb"], "5.00")
        self.assertEqual(f["purgeable_gb"], "0.10")
        self.assertEqual(f["filebacked_gb"], "3.00")
        self.assertEqual(f["vm_rss_gb"], "0.00")

    def test_resident_sizes_are_summed_by_process(self):
        r = run(PS)
        self.assertEqual(r.returncode, 0, r.stderr)
        f = self.fields(r.stdout)
        self.assertEqual(f["vm_rss_gb"], "1.59")
        self.assertEqual(f["llama_rss_gb"], "4.20")
        self.assertEqual(f["app_rss_gb"], "0.16")


if __name__ == "__main__":
    unittest.main()
