#!/bin/bash
# make backuptest: a disaster-recovery rehearsal on throwaway databases.
#   1. A project is moved into the store and used (store run 1).
#   2. It's backed up; a tampered copy of the backup is refused.
#   3. Disaster: the project folder, its database and its index are gone.
#   4. Restore; the app then finds everything (store run 2: chats, History,
#      notes, canon, findings, observer state; the index rebuilt).
#   5. Restoring over a database that has data is refused.
set -u
cd "$(dirname "$0")/.."
db=scribodo_test_bk$$
other=scribodo_test_bk$$_b
work=$PWD/build/ui/backup/work
bk=$PWD/build/ui/backup/bk
chmod -R u+w build/ui/backup 2>/dev/null; rm -rf build/ui/backup; mkdir -p "$work"
fail=0
step() { printf '%-60s' "$1"; }
ok() { if [ "$1" = 0 ]; then echo ok; else echo FAIL; fail=1; fi; }
run() { SCRIBODO_STORE=db SCRIBODO_DB_NAME=$1 SCRIBODO_SCRIPT=$2 SCRIBODO_SCRIPT_DIR=$work SCRIBODO_SHOT_DIR=$PWD/build/ui/backup \
        tools/watchdog.sh 180 backup ./build/scribodo 2>>build/ui/backup/stderr.log | grep -E "FAIL|passed" ; return "${PIPESTATUS[0]}"; }
cleanup() { tools/services.sh testdb drop $db >/dev/null 2>&1; tools/services.sh testdb drop $other >/dev/null 2>&1; }
trap cleanup EXIT

tools/services.sh testdb create $db >/dev/null || exit 1
step "project moved into the store and used"; out=$(run $db tests/ui/store/1_move_and_use.scribo); r=$?; ok $r; echo "$out" | sed 's/^/    /'
step "backup"; tools/backup.sh --db $db "$work/book" "$bk" >/dev/null; ok $?
step "backup verifies"; tools/backup.sh verify "$bk" >/dev/null; ok $?
step "backup holds the dump, the folder and its store marker"
[ -s "$bk/db.sql.gz" ] && [ -f "$bk/files/book/Chapter One.md" ] && [ -f "$bk/files/book/.scribodo/store.json" ]; ok $?
cp -R "$bk" "$bk-tampered"; printf 'x' >> "$bk-tampered/files/book/Chapter One.md"
step "a tampered backup is refused, nothing restored"
tools/restore.sh "$bk-tampered" --db $other --to "$work/never" >/dev/null 2>&1; r=$?; [ $r != 0 ] && [ ! -e "$work/never" ]; ok $?

# Disaster.
rm -rf "$work/book"
tools/services.sh testdb drop $db >/dev/null
step "restore (database recreated, folder back in place)"; tools/restore.sh "$bk" --db $db >/dev/null; ok $?
step "everything comes back, index rebuilt"; out=$(run $db tests/ui/store/2_resume.scribo); r=$?; ok $r; echo "$out" | sed 's/^/    /'

tools/services.sh testdb create $other >/dev/null
tools/restore.sh "$bk" --db $other --to "$work/copy1" >/dev/null 2>&1
step "restoring over a database with data is refused"
tools/restore.sh "$bk" --db $other --to "$work/copy2" >/dev/null 2>&1; r=$?; [ $r != 0 ] && [ ! -e "$work/copy2" ]; ok $?
exit $fail
