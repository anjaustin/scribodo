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
# The pinned Qdrant: a bundle's Contents/Resources/bin/qdrant (src/app/tools_dir.h lists what a bundle holds), else the
# source tree's build/qdrant/qdrant (make qdrant).
if [ -n "${SCRIBODO_QDRANT_BIN:-}" ]; then QDRANT_BIN="$SCRIBODO_QDRANT_BIN"
elif [ -x "$here/bin/qdrant" ]; then QDRANT_BIN="$here/bin/qdrant"
else QDRANT_BIN="$here/build/qdrant/qdrant"; fi
SCRIBODO_LOGS="${SCRIBODO_LOGS:-$HOME/Library/Logs/Scribodo}"

# mariadb_version_check MARIADBD: 0 when the server is the pinned release (SCRIBODO_MARIADB_VERSION) or a newer patch
# of its series, and then MARIADB_VERSION_WARNING names both versions (empty for the pinned release); 3, with the
# reason on stderr, for another series, an older patch, or a version that cannot be read. A release is three numbers of
# at most nine digits without leading zeros (an arithmetic test on 20 digits fails in `[`, which once read as "not
# older"); 11.4.013 is not one: mariadbd never prints it, so it is a misread or a mistyped pin, refused.
mariadb_version_check() {
    local found pin="$SCRIBODO_MARIADB_VERSION"
    MARIADB_VERSION_WARNING=""
    found="$("$1" --no-defaults --version 2>/dev/null | sed -n 's/.* Ver \([0-9][0-9.]*\)-MariaDB.*/\1/p')"
    local n='(0|[1-9][0-9]{0,8})' re
    re="^$n\\.$n\\.$n\$"
    if ! [[ "$pin" =~ $re ]]; then echo "SCRIBODO_MARIADB_VERSION is $pin, not a release such as 11.4.13" >&2; return 3; fi
    if ! [[ "$found" =~ $re ]] || [ "${found%.*}" != "${pin%.*}" ] || [ "${found##*.}" -lt "${pin##*.}" ]; then
        echo "MariaDB at $(dirname "$1") is ${found:-of unknown version}; the store needs $pin or a newer ${pin%.*} patch (brew install mariadb@${pin%.*})" >&2
        return 3
    fi
    [ "$found" = "$pin" ] ||
        MARIADB_VERSION_WARNING="MariaDB $found is running; the store is pinned to $pin (a newer ${pin%.*} patch is accepted until the pin is reviewed)"
}

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

# The root operations, written once for both modes (AUDIT-004 F6): one POSIX sh script, run on this Mac in native
# mode and inside the MariaDB container in docker mode. A prelude per mode gives it
#   ROOT_PW         the root password file
#   ACCOUNT_HOST    where a one-time account connects from (TCP natively, the socket in the container)
#   client TOOL …   a MariaDB client tool connected to this store (natively: no option files, --no-defaults first,
#                   so a ~/.my.cnf or Homebrew's my.cnf cannot redirect it)
# Passwords reach the clients through MYSQL_PWD, never argv, so never ps.
#   sql [client args]   stdin to the client as root
#   dump DATABASE       one consistent snapshot to stdout (mariadb-dump --single-transaction)
#   load DATABASE       stdin (plain mariadb-dump output) loaded by a one-time user that can reach only that database,
#                       in the client's sandbox (no shell, no file access); the user is dropped afterwards
root_ops_script() {
    cat <<'SH'
as_root() { t=$1; shift; MYSQL_PWD="$(cat "$ROOT_PW")" client "$t" -uroot "$@"; }
op=$1; shift
case $op in
    sql) as_root mariadb "$@" ;;
    dump) as_root mariadb-dump --single-transaction --quick --routines --triggers --hex-blob --default-character-set=utf8mb4 "$1" ;;
    load)
        db=$1; u=scribodo_restore_$$
        pw=$(head -c 18 /dev/urandom | od -An -tx1 | tr -d ' \n')
        printf "CREATE USER '%s'@'%s' IDENTIFIED BY '%s';\nGRANT ALL ON \`%s\`.* TO '%s'@'%s';\n" \
            "$u" "$ACCOUNT_HOST" "$pw" "$db" "$u" "$ACCOUNT_HOST" | as_root mariadb || exit 1
        rc=0
        MYSQL_PWD="$pw" client mariadb --sandbox -u"$u" "$db" || rc=$?
        printf "DROP USER IF EXISTS '%s'@'%s';\n" "$u" "$ACCOUNT_HOST" | as_root mariadb
        exit $rc ;;
    *) echo "unknown root operation $op" >&2; exit 2 ;;
esac
SH
}

root_op() { # root_op sql|dump|load ARGS: the script above, in this store's mode
    local m; m="$(services_mode)" || return 2
    if [ "$m" = native ]; then
        ROOT_PW="$SCRIBODO_SECRETS/db_root_password" ACCOUNT_HOST=127.0.0.1 MDB_DIR="$MARIADB_BIN" MDB_PORT="$SCRIBODO_DB_PORT" \
            /bin/sh -c 'client() { t=$1; shift; "$MDB_DIR/$t" --no-defaults -h127.0.0.1 -P"$MDB_PORT" --skip-ssl-verify-server-cert "$@"; }
'"$(root_ops_script)" sh "$@"
    else
        compose exec -T mariadb sh -c 'ROOT_PW=/run/secrets/db_root_password; ACCOUNT_HOST=localhost
client() { "$@"; }
'"$(root_ops_script)" sh "$@"
    fi
}
rootsql() { root_op sql "$@"; }
dump_db() { root_op dump "$1"; }
load_dump() { root_op load "$1"; }

# qdrant_curl [curl arguments] URL: a request to this store's Qdrant with its key, the one header the app also sends
# (src/llm/http.c); the key comes from a file descriptor, never argv.
qdrant_curl() {
    local key
    key="$(cat "$SCRIBODO_SECRETS/qdrant_api_key" 2>/dev/null)" && [ -n "$key" ] || return 1
    curl -s -H @<(printf 'Authorization: Bearer %s\n' "$key") "$@"
}

# qdrant_delete_collections PREFIX: drop every collection of a database ("<database>__<project>"); the index is
# derived and rebuilt from the store.
qdrant_delete_collections() {
    [ -s "$SCRIBODO_SECRETS/qdrant_api_key" ] || return 0
    for c in $(qdrant_curl "http://127.0.0.1:$SCRIBODO_QDRANT_PORT/collections" |
               python3 -I -c 'import json,sys; print(" ".join(c["name"] for c in json.load(sys.stdin)["result"]["collections"]))' 2>/dev/null); do
        case "$c" in "${1}__"*) qdrant_curl -o /dev/null -X DELETE "http://127.0.0.1:$SCRIBODO_QDRANT_PORT/collections/$c" ;; esac
    done
}
