# Shared by tools/services.sh, backup.sh, restore.sh and backuptest.sh: where Scribodo's services live and how a
# script reaches MariaDB as root without the password ever appearing on a command line.
#
# Two ways to run the services (the author's decision, 2026-11-14): `native`, MariaDB 11.4 from Homebrew and the
# pinned Qdrant release binary on this Mac, the recommended default on machines whose memory is for the model; or
# `docker`, the compose project in docker/compose.yml. Both bind 127.0.0.1 only and use the same generated credentials.
# How the mode is chosen (services_mode): SCRIBODO_SERVICES if set; else the mode file beside the secrets; else native
# if a native store already exists; else docker if credentials exist without a native store (someone who ran the
# containers before native mode existed: never start them on an empty native store, REDTEAM-023); else native when
# its binaries are installed, docker otherwise.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$(dirname "${BASH_SOURCE[0]}")/services.defaults"
export SCRIBODO_MARIADB_SERVER_FLAGS   # docker/compose.yml's MariaDB command
export PATH="$HOME/.docker/bin:/usr/local/bin:/opt/homebrew/bin:$PATH"
DEFAULT_SECRETS="$HOME/$SCRIBODO_DEFAULT_SECRETS_DIR"
export SCRIBODO_SECRETS="${SCRIBODO_SECRETS:-$DEFAULT_SECRETS}"
# One spelling of the folder for everything that names it (the pid files' argument markers, the socket's hash): a
# trailing slash, a doubled one or /tmp against /private/tmp would otherwise make a running store look stopped
# (REDTEAM-023).
if [ -d "$SCRIBODO_SECRETS" ]; then SCRIBODO_SECRETS="$(cd "$SCRIBODO_SECRETS" && pwd -P)"; export SCRIBODO_SECRETS; fi
export SCRIBODO_DB_PORT="${SCRIBODO_DB_PORT:-$SCRIBODO_DEFAULT_DB_PORT}"
export SCRIBODO_QDRANT_PORT="${SCRIBODO_QDRANT_PORT:-$SCRIBODO_DEFAULT_QDRANT_PORT}"
MARIADB_BIN="${SCRIBODO_MARIADB_BIN:-/opt/homebrew/opt/mariadb@11.4/bin}"
QDRANT_BIN="${SCRIBODO_QDRANT_BIN:-$here/build/qdrant/qdrant}"
SCRIBODO_LOGS="${SCRIBODO_LOGS:-$HOME/Library/Logs/Scribodo}"

native_store_exists() { [ -d "$SCRIBODO_SECRETS/mariadb/data/mysql" ]; }

services_mode() {
    case "${SCRIBODO_SERVICES:-}" in native|docker) echo "$SCRIBODO_SERVICES"; return ;; esac
    if [ -n "${SCRIBODO_SERVICES:-}" ]; then echo "SCRIBODO_SERVICES must be native or docker" >&2; return 2; fi
    if [ -s "$SCRIBODO_SECRETS/mode" ]; then
        case "$(cat "$SCRIBODO_SECRETS/mode")" in native) echo native; return ;; docker) echo docker; return ;; esac
    fi
    if native_store_exists; then echo native; return; fi
    if [ -s "$SCRIBODO_SECRETS/db_root_password" ]; then echo docker; return; fi
    if [ -x "$MARIADB_BIN/mariadbd" ] && [ -x "$QDRANT_BIN" ]; then echo native; else echo docker; fi
}

# The secrets folder is the author's own store when it is the default folder, however the path is spelled.
is_default_store() { [ "$SCRIBODO_SECRETS" -ef "$DEFAULT_SECRETS" ] || [ "$SCRIBODO_SECRETS" = "$DEFAULT_SECRETS" ]; }

compose() { docker compose -p "${SCRIBODO_COMPOSE_PROJECT:-scribodo}" -f "$here/docker/compose.yml" "$@"; }

# The MariaDB socket: beside the secrets when the path fits the Unix socket limit; else in a private directory under
# /tmp that this user owns, mode 0700, and that is not a link, or nothing (REDTEAM-023: a predictable shared path
# could be pre-created by another local user).
native_sock() {
    local s="$SCRIBODO_SECRETS/mariadb.sock"
    if [ ${#s} -le 100 ]; then echo "$s"; return; fi
    local d
    d="/tmp/scribodo-$(id -u)-$(printf '%s' "$SCRIBODO_SECRETS" | shasum | cut -c1-12)"
    mkdir -m 700 "$d" 2>/dev/null || true
    if [ -L "$d" ] || [ ! -d "$d" ] || [ ! -O "$d" ] || [ "$(stat -f %Lp "$d")" != 700 ]; then
        echo "the socket directory $d is not a private directory of this user; refusing" >&2; return 1
    fi
    echo "$d/mariadb.sock"
}

# The MariaDB clients read no option files (--no-defaults must come first): a ~/.my.cnf or Homebrew's my.cnf cannot
# redirect them or change what they do.
mdb() { local tool="$1"; shift; "$MARIADB_BIN/$tool" --no-defaults "$@"; }

# rootsql [mariadb client arguments]: stdin to the MariaDB client as root. The password comes from the secret file
# through the environment (never argv, so never in ps); in docker mode the container reads its own copy.
rootsql() {
    local m; m="$(services_mode)" || return 2
    case "$m" in
        native) MYSQL_PWD="$(cat "$SCRIBODO_SECRETS/db_root_password")" mdb mariadb -uroot -h127.0.0.1 -P"$SCRIBODO_DB_PORT" --skip-ssl-verify-server-cert "$@" ;;
        *) compose exec -T mariadb sh -c 'MYSQL_PWD="$(cat /run/secrets/db_root_password)" exec mariadb -uroot "$@"' sh "$@" ;;
    esac
}

# dump DATABASE: one consistent snapshot to stdout (mariadb-dump --single-transaction).
dump_db() {
    local m; m="$(services_mode)" || return 2
    case "$m" in
        native) MYSQL_PWD="$(cat "$SCRIBODO_SECRETS/db_root_password")" mdb mariadb-dump -uroot -h127.0.0.1 -P"$SCRIBODO_DB_PORT" --skip-ssl-verify-server-cert \
                    --single-transaction --quick --routines --triggers --hex-blob --default-character-set=utf8mb4 "$1" ;;
        *) compose exec -T mariadb sh -c 'MYSQL_PWD="$(cat /run/secrets/db_root_password)" exec mariadb-dump -uroot --single-transaction --quick --routines --triggers --hex-blob --default-character-set=utf8mb4 "$1"' sh "$1" ;;
    esac
}

# load_dump DATABASE: stdin (plain mariadb-dump output) loaded by a one-time user that can reach only that database,
# in the client's sandbox (no shell, no file access); the user is dropped afterwards.
load_dump() {
    local db="$1" m
    m="$(services_mode)" || return 2
    case "$m" in
        native)
            local u="scribodo_restore_$$" pw rc=0
            pw="$(head -c 18 /dev/urandom | od -An -tx1 | tr -d ' \n')"
            printf "CREATE USER '%s'@'127.0.0.1' IDENTIFIED BY '%s';\nGRANT ALL ON \`%s\`.* TO '%s'@'127.0.0.1';\n" "$u" "$pw" "$db" "$u" | rootsql
            MYSQL_PWD="$pw" mdb mariadb --sandbox -u"$u" -h127.0.0.1 -P"$SCRIBODO_DB_PORT" --skip-ssl-verify-server-cert "$db" || rc=$?
            printf "DROP USER IF EXISTS '%s'@'127.0.0.1';\n" "$u" | rootsql
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
