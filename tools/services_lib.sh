# Shared by tools/services.sh, backup.sh, restore.sh and backuptest.sh: where Scribodo's services live and how a
# script reaches MariaDB as root without the password ever appearing on a command line.
#
# Two ways to run the services (the author's decision, 2026-11-14): `native`, MariaDB 11.4 from Homebrew and the
# pinned Qdrant release binary on this Mac, the recommended default on machines whose memory is for the model (the
# Docker VM held about 1.9 GB beside services that use 0.3 GB); `docker`, the compose project in docker/compose.yml.
# SCRIBODO_SERVICES picks one; else the mode the last `services.sh up` wrote beside the secrets; else native when its
# binaries are present, docker otherwise. Both bind 127.0.0.1 only and use the same generated credentials.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$HOME/.docker/bin:/usr/local/bin:/opt/homebrew/bin:$PATH"
export SCRIBODO_SECRETS="${SCRIBODO_SECRETS:-$HOME/Library/Application Support/Scribodo/services}"
export SCRIBODO_DB_PORT="${SCRIBODO_DB_PORT:-33061}"
export SCRIBODO_QDRANT_PORT="${SCRIBODO_QDRANT_PORT:-6333}"
MARIADB_BIN="${SCRIBODO_MARIADB_BIN:-/opt/homebrew/opt/mariadb@11.4/bin}"
QDRANT_BIN="${SCRIBODO_QDRANT_BIN:-$here/build/qdrant/qdrant}"
SCRIBODO_LOGS="${SCRIBODO_LOGS:-$HOME/Library/Logs/Scribodo}"

services_mode() {
    if [ -n "${SCRIBODO_SERVICES:-}" ]; then echo "$SCRIBODO_SERVICES"; return; fi
    if [ -s "$SCRIBODO_SECRETS/mode" ]; then cat "$SCRIBODO_SECRETS/mode"; return; fi
    if [ -x "$MARIADB_BIN/mariadbd" ] && [ -x "$QDRANT_BIN" ]; then echo native; else echo docker; fi
}

compose() { docker compose -p "${SCRIBODO_COMPOSE_PROJECT:-scribodo}" -f "$here/docker/compose.yml" "$@"; }

# The MariaDB socket: beside the secrets when the path fits the Unix socket limit, else a short name under /tmp
# keyed on that folder (a scratch or runner folder can be long).
native_sock() {
    local s="$SCRIBODO_SECRETS/mariadb.sock"
    if [ ${#s} -le 100 ]; then echo "$s"; else echo "/tmp/scribodo-$(printf '%s' "$SCRIBODO_SECRETS" | shasum | cut -c1-12).sock"; fi
}

# rootsql [mariadb client arguments]: stdin to the MariaDB client as root. The password comes from the secret file
# through the environment (never argv, so never in ps); in docker mode the container reads its own copy.
rootsql() {
    case "$(services_mode)" in
        native) MYSQL_PWD="$(cat "$SCRIBODO_SECRETS/db_root_password")" "$MARIADB_BIN/mariadb" -uroot -h127.0.0.1 -P"$SCRIBODO_DB_PORT" "$@" ;;
        *) compose exec -T mariadb sh -c 'MYSQL_PWD="$(cat /run/secrets/db_root_password)" exec mariadb -uroot "$@"' sh "$@" ;;
    esac
}

# dump DATABASE: one consistent snapshot to stdout (mariadb-dump --single-transaction).
dump_db() {
    case "$(services_mode)" in
        native) MYSQL_PWD="$(cat "$SCRIBODO_SECRETS/db_root_password")" "$MARIADB_BIN/mariadb-dump" -uroot -h127.0.0.1 -P"$SCRIBODO_DB_PORT" \
                    --single-transaction --quick --routines --triggers --hex-blob --default-character-set=utf8mb4 "$1" ;;
        *) compose exec -T mariadb sh -c 'MYSQL_PWD="$(cat /run/secrets/db_root_password)" exec mariadb-dump -uroot --single-transaction --quick --routines --triggers --hex-blob --default-character-set=utf8mb4 "$1"' sh "$1" ;;
    esac
}

# load_dump DATABASE: stdin (plain mariadb-dump output) loaded by a one-time user that can reach only that database,
# in the client's sandbox (no shell, no file access); the user is dropped afterwards.
load_dump() {
    local db="$1"
    case "$(services_mode)" in
        native)
            local u="scribodo_restore_$$" pw rc=0
            pw="$(head -c 18 /dev/urandom | od -An -tx1 | tr -d ' \n')"
            printf "CREATE USER '%s'@'%%' IDENTIFIED BY '%s';\nGRANT ALL ON \`%s\`.* TO '%s'@'%%';\n" "$u" "$pw" "$db" "$u" | rootsql
            MYSQL_PWD="$pw" "$MARIADB_BIN/mariadb" --sandbox -u"$u" -h127.0.0.1 -P"$SCRIBODO_DB_PORT" "$db" || rc=$?
            printf "DROP USER IF EXISTS '%s'@'%%';\n" "$u" | rootsql
            return $rc ;;
        *)
            compose exec -T mariadb sh -c '
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
                exit $rc' sh "$db" ;;
    esac
}

# qdrant_delete_collections PREFIX: drop every collection of a database ("<database>__<project>"); the index is
# derived and rebuilt from the store.
qdrant_delete_collections() {
    local key
    key="$(cat "$SCRIBODO_SECRETS/qdrant_api_key" 2>/dev/null || true)"
    [ -n "$key" ] || return 0
    for c in $(curl -s -H @<(printf 'Authorization: Bearer %s\n' "$key") "http://127.0.0.1:$SCRIBODO_QDRANT_PORT/collections" |
               python3 -I -c 'import json,sys; print(" ".join(c["name"] for c in json.load(sys.stdin)["result"]["collections"]))' 2>/dev/null); do
        case "$c" in "${1}__"*) curl -s -o /dev/null -X DELETE -H @<(printf 'Authorization: Bearer %s\n' "$key") "http://127.0.0.1:$SCRIBODO_QDRANT_PORT/collections/$c" ;; esac
    done
}
