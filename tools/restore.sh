#!/bin/bash
# tools/restore.sh BACKUP_DIR --to FOLDER [--db NAME] [--replace]
#
# Restores a backup made by tools/backup.sh, for disasters (the database or
# the project folder lost). To get one project back without touching the
# database, use "Restore from a backup..." in the app instead.
#
#   - Checks the backup first: every file listed and unchanged, nothing
#     extra, no links (checksums detect damage, not deliberate tampering:
#     only restore backups you made). Nothing is touched if a check fails.
#   - Loads the dump into database NAME (default: the one it came from;
#     only "scribodo" or "scribodo_test_*"). It must be empty unless
#     --replace. --replace restores EVERY project in that database to the
#     time of the backup (the dump is the whole database). The dump must be
#     plain mariadb-dump output; it's loaded in the client's sandbox mode,
#     as a one-time user that can reach only that database.
#   - Copies the project folder to FOLDER, which must not exist. (FOLDER is
#     never taken from the backup itself.)
#   - The restored database's semantic index is dropped; the app rebuilds
#     it from the restored store.
# The app must not be running.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="$HOME/.docker/bin:/usr/local/bin:/opt/homebrew/bin:$PATH"
export SCRIBODO_SECRETS="${SCRIBODO_SECRETS:-$HOME/Library/Application Support/Scribodo/services}"
compose() { docker compose -f "$here/docker/compose.yml" "$@"; }
rootsql() { compose exec -T mariadb sh -c 'MYSQL_PWD="$(cat /run/secrets/db_root_password)" exec mariadb -uroot "$@"' sh "$@"; }

b="${1:?usage: $0 BACKUP_DIR --to FOLDER [--db NAME] [--replace]}"
shift
db="" to="" replace=0
while [ $# -gt 0 ]; do
    case "$1" in
        --db) db="${2:?name}"; shift 2 ;;
        --to) to="${2:?folder}"; shift 2 ;;
        --replace) replace=1; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -f "$b/manifest.json" ] || { echo "not a backup: $b" >&2; exit 2; }
"$here/tools/backup.sh" verify "$b" >/dev/null || { echo "the backup is damaged or has unexpected files; nothing was restored" >&2; exit 1; }
# Fields NUL-separated: folder names may hold spaces or anything else.
{ IFS= read -r -d '' m_db; IFS= read -r -d '' m_path; IFS= read -r -d '' m_name; } < <(python3 -I -c '
import json, sys
m = json.load(open(sys.argv[1]))
sys.stdout.write("\0".join([m.get("database") or "", m["project_path"], m["project_name"]]) + "\0")' "$b/manifest.json")
case "$m_name" in */*|.|..|"") echo "bad project name in the manifest" >&2; exit 1 ;; esac
if [ -z "$to" ]; then
    echo "say where the project folder goes: --to FOLDER (it was $m_path)" >&2; exit 2
fi
[ -e "$to" ] && { echo "already exists: $to (move it aside first)" >&2; exit 2; }
[ -d "$b/files/$m_name" ] || { echo "the backup has no project folder" >&2; exit 1; }
if pgrep -x scribodo >/dev/null 2>&1; then
    echo "quit Scribodo first (it would keep writing to the database being restored)" >&2; exit 2
fi
withdb=1
[ -f "$b/db.sql.gz" ] || withdb=0 # a project kept on files: nothing to load
db="${db:-$m_db}"
if [ "$withdb" = 1 ]; then
    [[ "$db" =~ ^scribodo(_test_[a-z0-9_]+)?$ ]] || { echo "restores go into \"scribodo\" or a scribodo_test_* database, not \"$db\"" >&2; exit 2; }
    # Only plain mariadb-dump output: its header and completion line.
    gzip -dc "$b/db.sql.gz" | head -3 | grep -q '^-- MariaDB dump ' || { echo "db.sql.gz isn't a mariadb-dump file" >&2; exit 1; }
    gzip -dc "$b/db.sql.gz" | tail -1 | grep -q '^-- Dump completed' || { echo "db.sql.gz is incomplete" >&2; exit 1; }
    tables="$(rootsql -N -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = '$db'")"
    if [ "$tables" != 0 ] && [ "$replace" != 1 ]; then
        echo "database $db already has data; --replace overwrites it (every project in it goes back to the backup)" >&2; exit 2
    fi
    printf 'CREATE DATABASE IF NOT EXISTS `%s` CHARACTER SET utf8mb4;\nGRANT ALL ON `%s`.* TO '"'"'scribodo'"'"'@'"'"'%%'"'"';\n' "$db" "$db" | rootsql
    # Loaded by a one-time user that can reach only this database, in the
    # client's sandbox (no \! shell, no file access). Its password is made
    # and used inside the container, never on a command line.
    gzip -dc "$b/db.sql.gz" | compose exec -T mariadb sh -c '
        set -e
        db=$1
        u=scribodo_restore_$$
        pw=$(head -c 18 /dev/urandom | od -An -tx1 | tr -d " \n")
        MYSQL_PWD="$(cat /run/secrets/db_root_password)" mariadb -uroot <<EOF
CREATE USER '"'"'$u'"'"'@'"'"'localhost'"'"' IDENTIFIED BY '"'"'$pw'"'"';
GRANT ALL ON \`$db\`.* TO '"'"'$u'"'"'@'"'"'localhost'"'"';
EOF
        rc=0
        MYSQL_PWD="$pw" mariadb --sandbox -u"$u" "$db" || rc=$?
        MYSQL_PWD="$(cat /run/secrets/db_root_password)" mariadb -uroot <<EOF
DROP USER IF EXISTS '"'"'$u'"'"'@'"'"'localhost'"'"';
EOF
        exit $rc' sh "$db"
    # The index described the database before; it's rebuilt from the store.
    key="$(cat "$SCRIBODO_SECRETS/qdrant_api_key" 2>/dev/null || true)"
    if [ -n "$key" ]; then
        for c in $(curl -s -H @<(printf 'Authorization: Bearer %s\n' "$key") http://127.0.0.1:6333/collections |
                   python3 -I -c 'import json,sys; print(" ".join(c["name"] for c in json.load(sys.stdin)["result"]["collections"]))' 2>/dev/null); do
            case "$c" in "${db}__"*) curl -s -o /dev/null -X DELETE -H @<(printf 'Authorization: Bearer %s\n' "$key") "http://127.0.0.1:6333/collections/$c" ;; esac
        done
    fi
fi

# The folder.
mkdir -p "$(dirname "$to")"
ditto "$b/files/$m_name" "$to"
if [ "$withdb" = 1 ]; then echo "restored database $db and $to"; else echo "restored $to"; fi
