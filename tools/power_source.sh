#!/bin/bash
# The Mac's power source, for the long measurements (GAP-003): prints one
# `POWER source=... charge=...` line and refuses (exit 3) to go on when the Mac
# is on battery, unless SCRIBODO_SOAK_ON_BATTERY=1. On 2026-11-13 the 60-minute
# real soak ran the battery to 1% and the Mac hibernated for 97 minutes in the
# middle of it (pmset -g log: "Entering Sleep state due to 'Low Power Sleep'");
# `caffeinate -i` holds off idle sleep, not low-power sleep, and a run on
# battery is also a run on a throttled machine. `--from FILE` reads a saved
# `pmset -g batt` output instead of asking pmset (tests).
set -euo pipefail
text=
if [[ ${1-} == --from && $# -eq 2 ]]; then text=$(cat "$2")
elif (( $# )); then echo 'usage: tools/power_source.sh [--from FILE]' >&2; exit 2
else text=$(pmset -g batt 2>/dev/null || true); fi
source=$(sed -n "s/.*Now drawing from '\([^']*\)'.*/\1/p" <<<"$text" | head -1)
charge=$(grep -o '[0-9]\{1,3\}%' <<<"$text" | head -1 || true)
echo "POWER source=${source:-unknown} charge=${charge:-unknown}"
if [[ $source == "Battery Power" && ${SCRIBODO_SOAK_ON_BATTERY-0} != 1 ]]; then
    echo "power_source: the Mac is on battery (${charge:-unknown}); a long run may hibernate mid-way and measures a throttled machine. Plug in, or SCRIBODO_SOAK_ON_BATTERY=1 to run anyway." >&2
    exit 3
fi
