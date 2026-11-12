#!/bin/sh
# The observer replay (tools/observer_replay.c) over the three test manuscripts with fixed canon, note and chat seeds,
# so two builds' observers can be compared byte for byte. usage: tools/observer_replay_run.sh <label> <binary>
# Writes build/observer/<label>-{pp,mars,heights}.txt (sorted findings) and .err (counts); the project copies are
# build/observer/<label>-<novel>/.
set -eu
label=$1; binary=$2
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/observer"; mkdir -p "$out"
run() {
    novel=$1; src=$2; shift 2
    dir="$out/$label-$novel"; rm -rf "$dir"; mkdir -p "$dir"; cp "$src"/*.md "$dir/"
    "$binary" "$dir" 900 "$@" > "$out/$label-$novel.txt" 2> "$out/$label-$novel.err" || echo "$novel exit=$?" >> "$out/$label-$novel.err"
}
# The canon seeds take the fake judge's shape ("<Name>'s <attr> is|are <value>") against a sentence the text has, so the
# conflict path runs; the note seeds repeat a question the text asks, so the overlap path runs; the chat seeds run the
# chat job (a question the chat asks is a finding; the second message has names).
run pp "$root/build/manuscripts/pride-and-prejudice/parts" \
    --canon "Wickham’s circumstances are fine." --canon "Elizabeth's eyes are blue." \
    --note "Wickham's circumstances are not so hopeless, surely." --note "Lydia is the youngest." \
    --chat "Should Elizabeth know about the letter before Jane does?" --chat "Ingrid met Dvořák at the quay." &
run mars "$root/build/manuscripts/a-princess-of-mars" \
    --canon "Dejah Thoris is the daughter of Tardos Mors." --canon "Sola's eyes are blue." \
    --note "Lorquas Ptomel, Jed, intend holding her for ransom?" \
    --chat "Does John Carter know Sola is Tars Tarkas's daughter?" &
run heights "$root/build/manuscripts/wuthering-heights" \
    --canon "Hareton Earnshaw is Heathcliff's son." --canon "Catherine's eyes are blue." \
    --note "Is there nobody inside to open the door?" \
    --chat "Should Lockwood learn who Hareton's father is?" &
wait
for novel in pp mars heights; do printf '%s: ' "$novel"; cat "$out/$label-$novel.err"; done
