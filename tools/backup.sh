#!/bin/bash
# tools/backup.sh [--db NAME] PROJECT_DIR [DEST]
# tools/backup.sh verify BACKUP_DIR
#
# A backup (SOW §7) is two things taken together:
#   db.sql.gz   a transactionally consistent dump of the project store
#               (mariadb-dump --single-transaction: one snapshot, no locks;
#               the whole database, so every project's rows stay consistent
#               with each other);
#   files/      a copy of the project folder (documents and .scribodo/).
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
compose() { docker compose -f "$here/docker/compose.yml" "$@"; }

if [ "${1:-}" = verify ]; then
    b="${2:?backup folder}"
    [ -f "$b/SHA256SUMS" ] || { echo "not a backup: $b" >&2; exit 2; }
    (cd "$b" && shasum -a 256 -c --quiet SHA256SUMS) && echo "backup verified: $b"
    exit
fi

db=scribodo
if [ "${1:-}" = --db ]; then db="${2:?name}"; shift 2; fi
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
compose exec -T mariadb sh -c \
    'MYSQL_PWD="$(cat /run/secrets/db_root_password)" exec mariadb-dump -uroot --single-transaction --quick --routines --triggers --hex-blob --default-character-set=utf8mb4 "$1"' \
    sh "$db" | gzip -9 > "$dest/db.sql.gz.part"
# a dump that ended early has no completion line
gzip -dc "$dest/db.sql.gz.part" | tail -1 | grep -q '^-- Dump completed' || { echo "the database dump is incomplete" >&2; exit 1; }
mv "$dest/db.sql.gz.part" "$dest/db.sql.gz"

# 2. The project folder, with its metadata.
ditto "$project" "$dest/files/$name"

# 3. What this is, and checksums of everything.
pid=""
[ -f "$project/.scribodo/store.json" ] &&
    pid="$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1])).get("project_id",""))' "$project/.scribodo/store.json" 2>/dev/null || true)"
schema="$(compose exec -T mariadb sh -c 'MYSQL_PWD="$(cat /run/secrets/db_root_password)" exec mariadb -uroot -N -e "SELECT MAX(version) FROM schema_migrations" "$1"' sh "$db" 2>/dev/null || echo "")"
python3 -I - "$dest/manifest.json" "$stamp" "$db" "$project" "$name" "$pid" "$schema" <<'EOF'
import json, sys
out, stamp, db, path, name, pid, schema = sys.argv[1:]
json.dump({"format": 1, "created": stamp, "database": db, "project_path": path, "project_name": name,
           "project_id": pid, "schema_version": int(schema) if schema.strip().isdigit() else None,
           "contents": {"db.sql.gz": "mariadb-dump --single-transaction of the database",
                        "files/" + name: "the project folder"}}, open(out, "w"), indent=2)
EOF
(cd "$dest" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 shasum -a 256 > SHA256SUMS)
echo "$dest"
