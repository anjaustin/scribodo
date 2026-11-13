#!/bin/bash
# A series of manuscript baselines (GAP-003 repeatability): tools/manuscript_baseline.sh run N times, each on its
# own disposable database and project, every log kept, then tabulated by tools/manuscript_series.py. Not an SLA;
# the spread across runs is what a budget is read against.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
usage() {
    echo 'usage: tools/manuscript_series.sh [--runs N] [--out DIR] [-- baseline arguments]'
    echo '--runs N      how many baselines to run (default 5, 1 to 50).'
    echo '--out DIR     where run-N.log, series.md and series.json go (default build/gap003/series-<stamp>).'
    echo 'Arguments after -- are passed to tools/manuscript_baseline.sh (for example --real, --manuscript DIR).'
    echo 'Requires tools/services.sh up and a built app; an idle machine for the numbers to mean anything.'
}
runs=5; out=
while (( $# )); do
    case "$1" in
        --help) usage; exit 0 ;;
        --runs) [[ $# -ge 2 && $2 =~ ^[1-9][0-9]?$ && $2 -le 50 ]] || { usage >&2; exit 2; }; runs=$2; shift 2 ;;
        --out) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; out=$2; shift 2 ;;
        --) shift; break ;;
        *) usage >&2; exit 2 ;;
    esac
done
cd "$here"
[[ -n $out ]] || out="build/gap003/series-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
echo "manuscript_series: runs=$runs out=$out baseline_args=[$*] tree=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)" | tee "$out/series.log"
failed=0
for (( i = 1; i <= runs; i++ )); do
    echo "manuscript_series: run $i of $runs $(date -u +%Y-%m-%dT%H:%M:%SZ)" | tee -a "$out/series.log"
    if ! tools/manuscript_baseline.sh "$@" > "$out/run-$i.log" 2>&1; then
        failed=$((failed + 1))
        echo "manuscript_series: run $i exited non-zero (see $out/run-$i.log)" | tee -a "$out/series.log"
    fi
    grep -h "FRAME_METRICS\|FRAME_SOAK_RSS\|passed, .* failed\|process_wall_seconds=" "$out/run-$i.log" | sed 's/ (includes.*//' | cut -c1-140 | tee -a "$out/series.log"
done
python3 tools/manuscript_series.py "$out" --json "$out/series.json" | tee "$out/series.md" | tee -a "$out/series.log"
echo "manuscript_series: failed_runs=$failed" | tee -a "$out/series.log"
[[ $failed == 0 ]]
