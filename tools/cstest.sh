#!/bin/bash
# Rename safety on a case-sensitive volume (red-team 006): builds a small
# case-sensitive APFS image, runs tests/ui/case_sensitive.scribo on it, and
# detaches it. Needs no root.
set -u
cd "$(dirname "$0")/.."
img=build/cs.sparseimage; mnt=build/csvol
rm -rf "$img" "$mnt"; mkdir -p "$mnt"
hdiutil create -quiet -size 20m -fs "Case-sensitive APFS" -type SPARSE -volname scribodo-cs "$img" || exit 1
hdiutil attach -quiet -mountpoint "$PWD/$mnt" "$img" || exit 1
SCRIBODO_SCRIPT=tests/ui/case_sensitive.scribo SCRIBODO_SCRIPT_DIR="$PWD/$mnt" SCRIBODO_SHOT_DIR="$PWD/build" \
    tools/watchdog.sh 120 ui-case ./build/scribodo 2>build/cstest.log
rc=$?
hdiutil detach -quiet "$mnt"
rm -f "$img"
exit $rc
