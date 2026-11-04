#!/bin/bash
# tools/restore.sh BACKUP_DIR [--db NAME] [--to FOLDER] [--replace]
#
# Restores a backup made by tools/backup.sh:
#   - checks every checksum first (nothing is touched if one fails);
#   - loads the dump into database NAME (default: the one it came from),
#     which must be empty unless --replace;
#   - copies the project folder to FOLDER (default: where it came from),
#     which must not exist (move a damaged one aside first).
# The app then finds the project at its folder. A restored folder whose
# original still exists elsewhere with the same project is treated as a
# copy (and can be made its own project). The semantic index is rebuilt by
# the app when it finds the project's index missing.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="$HOME/.docker/bin:/usr/local/bin:/opt/homebrew/bin:$PATH"
export SCRIBODO_SECRETS="${SCRIBODO_SECRETS:-$HOME/Library/Application Support/Scribodo/services}"
compose() { docker compose -f "$here/docker/compose.yml" "$@"; }
rootsql() { compose exec -T mariadb sh -c 'MYSQL_PWD="$(cat /run/secrets/db_root_password)" exec mariadb -uroot "$@"' sh "$@"; }

b="${1:?usage: $0 BACKUP_DIR [--db NAME] [--to FOLDER] [--replace]}"
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
"$here/tools/backup.sh" verify "$b" >/dev/null || { echo "the backup is damaged (checksums differ); nothing was restored" >&2; exit 1; }
# Fields NUL-separated: folder names may hold spaces or anything else.
{ IFS= read -r -d '' m_db; IFS= read -r -d '' m_path; IFS= read -r -d '' m_name; } < <(python3 -I -c '
import json, sys
m = json.load(open(sys.argv[1]))
sys.stdout.write("\0".join([m["database"], m["project_path"], m["project_name"]]) + "\0")' "$b/manifest.json")
case "$m_name" in */*|.|..|"") echo "bad project name in the manifest" >&2; exit 1 ;; esac
db="${db:-$m_db}"
to="${to:-$m_path}"
[[ "$db" =~ ^[a-z][a-z0-9_]*$ ]] || { echo "bad database name: $db" >&2; exit 2; }
[ -e "$to" ] && { echo "already exists: $to (move it aside first)" >&2; exit 2; }
[ -d "$b/files/$m_name" ] || { echo "the backup has no project folder" >&2; exit 1; }

# The database: created if missing, refused if it has tables (unless --replace).
tables="$(rootsql -N -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = '$db'")"
if [ "$tables" != 0 ] && [ "$replace" != 1 ]; then
    echo "database $db already has data; pass --replace to overwrite it" >&2; exit 2
fi
printf 'CREATE DATABASE IF NOT EXISTS `%s` CHARACTER SET utf8mb4;\nGRANT ALL ON `%s`.* TO '"'"'scribodo'"'"'@'"'"'%%'"'"';\n' "$db" "$db" | rootsql
gzip -dc "$b/db.sql.gz" | rootsql "$db"

# The folder.
mkdir -p "$(dirname "$to")"
ditto "$b/files/$m_name" "$to"
echo "restored database $db and $to"
