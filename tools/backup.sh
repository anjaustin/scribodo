#!/bin/bash
# tools/backup.sh [--db NAME | --no-db] [--with EXPORT_DIR] PROJECT_DIR [DEST]
# tools/backup.sh verify BACKUP_DIR
#
# A backup (SOW §7) is two things taken together:
#   db.sql.gz   a transactionally consistent dump of the project store
#               (mariadb-dump --single-transaction: one snapshot, no locks;
#               the whole database, so every project's rows stay consistent
#               with each other);
#   files/      a copy of the project folder (documents and .scribodo/);
#   export/     (--with, from the app) the project's own data from the store
#               as files with original times, so the app can restore just
#               this project into a new folder without touching the database.
# --no-db: a project kept on files (no store), so there is nothing to dump.
# plus manifest.json and SHA256SUMS. Qdrant isn't backed up: it's rebuilt
# from the store (acceptance 10), which the app does when a project's index
# is missing.
#
# DEST defaults to ~/Library/Application Support/Scribodo/backups/
# <project>-<time>. Restore with tools/restore.sh.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="$HOME/.docker/bin:/usr/local/bin:/opt/homebrew/bin:$PATH"
export SCRIBODO_SECRETS="${SCRIBODO_SECRETS:-$HOME/Library/Application Support/Scribodo/services}"
compose() { docker compose -p "${SCRIBODO_COMPOSE_PROJECT:-scribodo}" -f "$here/docker/compose.yml" "$@"; }

# verify: every listed file present and unchanged, and nothing else: no
# extra files, no links or other non-regular entries. (Checksums kept in
# the backup detect damage, not deliberate tampering: only restore backups
# you made.)
if [ "${1:-}" = verify ]; then
    b="${2:?backup folder}"
    [ -f "$b/SHA256SUMS" ] || { echo "not a backup: $b" >&2; exit 2; }
    cd "$b"
    if [ -n "$(find . ! -type f ! -type d -print -quit)" ]; then
        echo "the backup holds links or special files: $(find . ! -type f ! -type d -print -quit)" >&2; exit 1
    fi
    shasum -a 256 -c --quiet SHA256SUMS || exit 1
    python3 -I - <<'PY' || exit 1
import os, sys
listed = set()
for line in open("SHA256SUMS", "rb"):
    line = line.rstrip(b"\n")
    if b"  " not in line: sys.exit("SHA256SUMS is malformed")
    name = line.split(b"  ", 1)[1]
    if line.startswith(b"\\"):  # shasum escapes names holding \n or \\
        name = name.replace(b"\\\\", b"\0").replace(b"\\n", b"\n").replace(b"\0", b"\\")
    listed.add(name)
present = set()
for root, dirs, files in os.walk(b"."):
    for f in files:
        p = os.path.join(root, f)
        if p != b"./SHA256SUMS": present.add(p)
extra = sorted(present - listed)
if extra: sys.exit("files not in the backup's list: " + ", ".join(e.decode("utf-8", "replace") for e in extra[:5]))
PY
    echo "backup verified: $b"
    exit
fi

db=scribodo with="" nodb=0
while [ $# -gt 0 ]; do
    case "$1" in
        --db) db="${2:?name}"; shift 2 ;;
        --no-db) nodb=1; shift ;;
        --with) with="${2:?folder}"; shift 2 ;;
        *) break ;;
    esac
done
[ -z "$with" ] || [ -d "$with" ] || { echo "no such folder: $with" >&2; exit 2; }
# The database name reaches the dump command: letters, digits, underscore.
[[ "$db" =~ ^[a-z][a-z0-9_]*$ ]] || { echo "bad database name: $db" >&2; exit 2; }
project="${1:?usage: $0 [--db NAME] PROJECT_DIR [DEST]}"
[ -d "$project" ] || { echo "no such folder: $project" >&2; exit 2; }
project="$(cd "$project" && pwd -P)"
name="$(basename "$project")"
stamp="$(date +%Y%m%d-%H%M%S)"
dest="${2:-$HOME/Library/Application Support/Scribodo/backups/$name-$stamp}"
[ -e "$dest" ] && { echo "already exists: $dest" >&2; exit 2; }
mkdir -p "$dest/files"
chmod 700 "$dest"
umask 077

# 1. The store: one consistent snapshot. The root password is read inside
# the container (never on a command line); the dump streams out compressed.
if [ "$nodb" = 0 ]; then
compose exec -T mariadb sh -c \
    'MYSQL_PWD="$(cat /run/secrets/db_root_password)" exec mariadb-dump -uroot --single-transaction --quick --routines --triggers --hex-blob --default-character-set=utf8mb4 "$1"' \
    sh "$db" | gzip -9 > "$dest/db.sql.gz.part"
# a dump that ended early has no completion line
gzip -dc "$dest/db.sql.gz.part" | tail -1 | grep -q '^-- Dump completed' || { echo "the database dump is incomplete" >&2; exit 1; }
mv "$dest/db.sql.gz.part" "$dest/db.sql.gz"
fi

# 2. The project folder, with its metadata.
ditto "$project" "$dest/files/$name"
[ -z "$with" ] || { mkdir -p "$dest/export"; ditto "$with" "$dest/export/$name"; }

# 3. What this is, and checksums of everything.
pid=""
[ -f "$project/.scribodo/store.json" ] &&
    pid="$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1])).get("project_id",""))' "$project/.scribodo/store.json" 2>/dev/null || true)"
schema=""
[ "$nodb" = 1 ] || schema="$(compose exec -T mariadb sh -c 'MYSQL_PWD="$(cat /run/secrets/db_root_password)" exec mariadb -uroot -N -e "SELECT MAX(version) FROM schema_migrations" "$1"' sh "$db" 2>/dev/null || echo "")"
python3 -I - "$dest/manifest.json" "$stamp" "$db" "$project" "$name" "$pid" "$schema" "$nodb" "${with:+1}" <<'EOF'
import json, sys
out, stamp, db, path, name, pid, schema, nodb, export = sys.argv[1:]
contents = {"files/" + name: "the project folder"}
if nodb != "1": contents["db.sql.gz"] = "mariadb-dump --single-transaction of the database"
if export: contents["export/" + name] = "the project's store data as files (in-app restore)"
json.dump({"format": 1, "created": stamp, "database": None if nodb == "1" else db, "project_path": path,
           "project_name": name, "project_id": pid,
           "schema_version": int(schema) if schema.strip().isdigit() else None, "contents": contents},
          open(out, "w"), indent=2)
EOF
(cd "$dest" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 shasum -a 256 > SHA256SUMS)
echo "$dest"
