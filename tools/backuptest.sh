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
export PATH="$HOME/.docker/bin:$PATH"
compose() { docker compose -p "${SCRIBODO_COMPOSE_PROJECT:-scribodo}" -f docker/compose.yml "$@"; }
saved_receipts() {
    compose exec -T mariadb sh -c 'MYSQL_PWD="$(cat /run/secrets/db_root_password)" mariadb -uroot -N "$1" -e "SELECT CONCAT(d.id, CHAR(58), r.id, CHAR(58), r.source_hash) FROM documents d JOIN revisions r ON r.document_id=d.id AND r.id=d.saved_revision_id ORDER BY d.id LIMIT 16"' sh "$1"
}
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
step "a saved revision receipt exists before backup"
saved_before=$(saved_receipts "$db"); r=$?; [ "$r" = 0 ] && [ -n "$saved_before" ]; ok $?
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
step "a restore needs --to (never the backup's own path)"
tools/restore.sh "$bk" --db $db >/dev/null 2>&1; r=$?; [ $r != 0 ] && [ ! -e "$work/book" ]; ok $?
step "restore (database recreated, folder back in place)"; tools/restore.sh "$bk" --db $db --to "$work/book" >/dev/null; ok $?
step "saved revision identity and SHA-256 survive restore"
saved_after=$(saved_receipts "$db"); r=$?; [ "$r" = 0 ] && [ -n "$saved_after" ] && [ "$saved_before" = "$saved_after" ]; ok $?
step "everything comes back, index rebuilt"; out=$(run $db tests/ui/store/2_resume.scribo); r=$?; ok $r; echo "$out" | sed 's/^/    /'

tools/services.sh testdb create $other >/dev/null
tools/restore.sh "$bk" --db $other --to "$work/copy1" >/dev/null 2>&1
step "restoring over a database with data is refused without --replace"
tools/restore.sh "$bk" --db $other --to "$work/copy2" >/dev/null 2>&1; r=$?; [ $r != 0 ] && [ ! -e "$work/copy2" ]; ok $?
# Red-team 009: crafted backups.
craft() { rm -rf "$bk-craft"; cp -R "$bk" "$bk-craft"; }
resum() { (cd "$bk-craft" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 shasum -a 256 > SHA256SUMS); }
craft; echo x > "$bk-craft/files/book/planted.md"
step "a file not in the backup's list is refused"; tools/restore.sh "$bk-craft" --db $other --replace --to "$work/c1" >/dev/null 2>&1; r=$?; [ $r != 0 ] && [ ! -e "$work/c1" ]; ok $?
craft; ln -s /etc/hosts "$bk-craft/files/book/link.md"; resum
step "a link in the backup is refused"; tools/restore.sh "$bk-craft" --db $other --replace --to "$work/c2" >/dev/null 2>&1; r=$?; [ $r != 0 ] && [ ! -e "$work/c2" ]; ok $?
dbexists() { compose exec -T mariadb sh -c 'MYSQL_PWD="$(cat /run/secrets/db_root_password)" mariadb -uroot -N -e "SHOW DATABASES LIKE \"$1\""' sh "$1" | grep -c .; }
craft; printf -- '-- MariaDB dump 10.19\nCREATE DATABASE scribodo_test_pwned;\n-- Dump completed\n' | gzip > "$bk-craft/db.sql.gz"; resum
step "a dump can't create another database"
tools/restore.sh "$bk-craft" --db $other --replace --to "$work/c3" >/dev/null 2>&1; r=$?; [ $r != 0 ] && [ "$(dbexists scribodo_test_pwned)" = 0 ] && [ ! -e "$work/c3" ]; ok $?
tools/services.sh testdb drop scribodo_test_pwned >/dev/null 2>&1
# (\! would run inside the database container: that's where to look.)
incontainer() { compose exec -T mariadb sh -c "$1"; }
incontainer 'rm -f /tmp/scribodo_pwned'
craft; printf -- '-- MariaDB dump 10.19\n\\! touch /tmp/scribodo_pwned\n-- Dump completed\n' | gzip > "$bk-craft/db.sql.gz"; resum
step "a dump with a shell escape is refused"
tools/restore.sh "$bk-craft" --db $other --replace --to "$work/c3b" >/dev/null 2>&1; r=$?
[ $r != 0 ] && ! incontainer 'test -e /tmp/scribodo_pwned' && [ ! -e "$work/c3b" ]; ok $?
craft; printf -- 'DROP TABLE projects;\n' | gzip > "$bk-craft/db.sql.gz"; resum
step "a dump that isn't mariadb-dump output is refused"; tools/restore.sh "$bk-craft" --db $other --replace --to "$work/c4" >/dev/null 2>&1; r=$?; [ $r != 0 ]; ok $?
step "a database other than scribodo[_test_*] is refused"; tools/restore.sh "$bk" --db mysql --replace --to "$work/c5" >/dev/null 2>&1; r=$?; [ $r != 0 ]; ok $?
# The index of a restored database is dropped (it described later data).
key="$(cat "${SCRIBODO_SECRETS:-$HOME/Library/Application Support/Scribodo/services}/qdrant_api_key")"
qport="${SCRIBODO_QDRANT_PORT:-6333}"
curl -s -o /dev/null -X PUT -H @<(printf 'Authorization: Bearer %s\n' "$key") -H 'Content-Type: application/json' \
    -d '{"vectors":{"size":4,"distance":"Cosine"}}' "http://127.0.0.1:$qport/collections/${other}__pstale"
tools/restore.sh "$bk" --db $other --replace --to "$work/c6" >/dev/null 2>&1
left=$(curl -s -H @<(printf 'Authorization: Bearer %s\n' "$key") "http://127.0.0.1:$qport/collections" | grep -c "${other}__pstale")
step "--replace drops the restored database's stale index"; [ "$left" = 0 ]; ok $?
exit $fail
