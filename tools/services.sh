#!/bin/bash
# tools/services.sh up|down|status|logs|purge|mode|testdb create|drop|sql NAME|rootsql [args]|dump DB|rotate-root|migrate-from-docker
#
# Starts Scribodo's local services (MariaDB 11.4, Qdrant 1.20) natively on this Mac or in Docker Compose
# (tools/services_lib.sh says how the mode is chosen; SCRIBODO_SERVICES=native|docker forces one for this call without
# changing the remembered mode). Credentials are generated once into a 0700 folder in Application Support and never
# printed. Everything binds to 127.0.0.1.
#
#   up                   start the services (the app runs this); prints their status
#   down                 stop them; the data stays
#   status | logs        what runs where | the last 50 lines of each log
#   mode                 native or docker, as this call would use
#   purge                stop and delete the store (refused for the author's store without SCRIBODO_PURGE_CONFIRM=1)
#   testdb create|drop|sql NAME   a throwaway scribodo_test_* database for tests (sql: stdin against it)
#   rootsql [args] | dump DB      stdin to the MariaDB client as root | one consistent dump to stdout
#   rotate-root          a new MariaDB root password
#   migrate-from-docker  move the containers' store to the native services (see the function for its guarantees)
#
# Variables (every one optional; defaults in tools/services.defaults and below; README.md, "Environment variables"):
#   SCRIBODO_SERVICES          native|docker: the mode for this call only, never remembered
#   SCRIBODO_SECRETS           the store's folder: credentials, the native data, the remembered mode
#                              (~/ then SCRIBODO_DEFAULT_SECRETS_DIR in tools/services.defaults)
#   SCRIBODO_DB_PORT           MariaDB's port on 127.0.0.1 (SCRIBODO_DEFAULT_DB_PORT)
#   SCRIBODO_QDRANT_PORT       Qdrant's port on 127.0.0.1 (SCRIBODO_DEFAULT_QDRANT_PORT)
#   SCRIBODO_LOGS              the native daemons' logs (~/Library/Logs/Scribodo)
#   SCRIBODO_MARIADB_BIN       the folder with mariadbd and its clients (/opt/homebrew/opt/mariadb@11.4/bin)
#   SCRIBODO_QDRANT_BIN        the Qdrant executable (a bundle's Resources/bin/qdrant, else build/qdrant/qdrant)
#   SCRIBODO_COMPOSE_PROJECT   docker mode's Compose project name (scribodo); a throwaway store uses its own
#   SCRIBODO_MARIADB_MEM_LIMIT docker mode's memory cap for MariaDB (768m)
#   SCRIBODO_QDRANT_MEM_LIMIT  docker mode's memory cap for Qdrant (1g)
#   SCRIBODO_NATIVE_FRESH      1: `up` starts an empty native store over credentials whose store is in the containers
#   SCRIBODO_MIGRATE_REPLACE   1: migrate-from-docker replaces an existing native store, keeping a dump of it first
#   SCRIBODO_PURGE_CONFIRM     1: purge may delete the author's own store
#   SCRIBODO_MARIADB_VERSION   the MariaDB release the store is pinned to (tools/services.defaults); native mode takes
#                              it or a newer patch of its series
# Set by the scripts themselves, not by hand:
#   SCRIBODO_SERVICES_LOCKED   marks the re-run under the store's lock (lockf), so it does not lock again
#   SCRIBODO_SERVICES_LOCK_WAIT  seconds to wait for the store's lock (300); tests shorten it
#   SCRIBODO_MIGRATE_ASSUME_FREE_KB  the free space the migration's check assumes (tests; the real check uses df)
#   SCRIBODO_MARIADB_SERVER_FLAGS  the MariaDB server's settings from tools/services.defaults, exported for Compose
#
# Exit codes: the contract the app reads (src/app/service_supervisor.h, SERVICES_EXIT_*; tests/test_services.c pins
# both). The last line on stderr says what happened in words; the app shows it beside the code's sentence.
#   0   done
#   1   failed for any other reason
#   2   refused: the decision is the author's (`up` over credentials whose store is still in the containers:
#       migrate-from-docker moves it; a purge or migration guard; a usage error)
#   3   MariaDB is not installed, or is not the version the store is pinned to
#   4   Qdrant is not built (make qdrant)
#   5   a service's port is taken by another program
#   6   docker mode, and Docker is not running
#   75  another services.sh held this store's lock for five minutes (lockf's EX_TEMPFAIL)
# On success, a last stderr line beginning `warning: ` is for the author (the app shows it once): today, a newer MariaDB
# patch than the pin.
set -euo pipefail
# Output that cannot be written never stops the script half-way through a migration: a stdout or stderr the caller
# closed is reopened on /dev/null (otherwise the next file opened would become descriptor 1 and receive the progress
# lines), and a pipe whose reader has gone makes a write fail (EPIPE, which the progress lines ignore) instead of
# killing the script with SIGPIPE before it can roll back (verification of AUDIT-004, 2026-11-16).
{ exec 9>&1; } 2>/dev/null || exec 1>/dev/null   # duplicating a closed descriptor fails; `true >&1` would not
{ exec 9>&2; } 2>/dev/null || exec 2>/dev/null
exec 9>&-
trap '' PIPE
# say TEXT: a progress line that may fail to print without consequence. Printed from a subshell: a builtin echo that
# fails keeps its unwritten text in bash's buffer, and the next subshell forked (a command substitution) would flush
# it into its own output (seen 2026-11-16: the line landed inside the MariaDB version check).
say() { ( printf '%s\n' "$*" ) 2>/dev/null || true; }
. "$(dirname "$0")/services_lib.sh"

# One mutating command at a time per store: the app's supervisor and a terminal can both run `up` (REDTEAM-023).
case "${1:-status}" in
    up|down|purge|migrate-from-docker|rotate-root)
        if [ -z "${SCRIBODO_SERVICES_LOCKED:-}" ]; then
            mkdir -p "$SCRIBODO_SECRETS" && chmod 700 "$SCRIBODO_SECRETS"
            SCRIBODO_SERVICES_LOCKED=1 exec /usr/bin/lockf -k -t "${SCRIBODO_SERVICES_LOCK_WAIT:-300}" "$SCRIBODO_SECRETS/.services.lock" "$0" "$@"
        fi ;;
esac

# Every exit the script means goes through `leave`; any other way out is a failure and leaves as 1: a command failing
# under `set -e` with its own code (curl's 7, docker compose's 4, anything's 75, which would pose as the contract's),
# and an expansion error under `set -u`, which reaches this trap with $? = 0 and would look like success (verification
# of the red-team pass). 75 only ever comes from lockf, the outer process, never from this script.
finished=""
leave() { finished=1; exit "$1"; }
trap '[ -n "$finished" ] || exit 1' EXIT   # bash has already said why on stderr; that line stays the last
usage() { echo "usage: $0 up|down|purge|status|logs|mode|testdb create|drop|sql NAME|rootsql [args]|dump DB|rotate-root|migrate-from-docker" >&2; leave 2; }

had_credentials=0
[ -s "$SCRIBODO_SECRETS/db_root_password" ] && had_credentials=1

secrets() {
    mkdir -p "$SCRIBODO_SECRETS"
    chmod 700 "$SCRIBODO_SECRETS"
    umask 077
    # Each secret is checked: callers run this on the left of `||`, where `set -e` does not apply, and an empty file
    # would become an empty root password or API key (REDTEAM-023).
    gen() { local f="$SCRIBODO_SECRETS/$1"; [ -s "$f" ] || { openssl rand -hex 24 > "$f" && [ -s "$f" ]; } || { echo "could not create $1" >&2; return 1; }; }
    gen db_password || return 1
    gen db_root_password || return 1
    gen qdrant_api_key || return 1
    printf 'QDRANT__SERVICE__API_KEY=%s\n' "$(cat "$SCRIBODO_SECRETS/qdrant_api_key")" > "$SCRIBODO_SECRETS/qdrant.env" || return 1
    if [ "$1" = docker ]; then
        # The containers read the secret files as another user: readable by the
        # Docker VM, still inside a 0700 folder on this Mac.
        chmod 644 "$SCRIBODO_SECRETS/db_password" "$SCRIBODO_SECRETS/db_root_password"
    else
        chmod 600 "$SCRIBODO_SECRETS/db_password" "$SCRIBODO_SECRETS/db_root_password"
    fi
}

# The remembered mode changes only when no mode was forced for this call (REDTEAM-023: a one-off
# `SCRIBODO_SERVICES=docker up` used to switch the app back to the stale containers).
remember_mode() { [ -n "${SCRIBODO_SERVICES:-}" ] || write_mode "$1"; }
# write_mode MODE: the mode file replaced whole (a temporary file renamed over it), 0600 like the secrets beside it.
write_mode() {
    local t
    t="$(umask 077; mktemp "$SCRIBODO_SECRETS/.mode.XXXXXX")" || return 1
    { printf '%s\n' "$1" > "$t" && mv -f "$t" "$SCRIBODO_SECRETS/mode"; } || { rm -f "$t"; return 1; }
}

# ---- native --------------------------------------------------------------------------------------------------------
mdata="$SCRIBODO_SECRETS/mariadb"
qdata="$SCRIBODO_SECRETS/qdrant"
sock="$(native_sock)"
mpid="$mdata/mariadbd.pid"
qpid="$qdata/qdrant.pid"

# A pid file is trusted only when its process is still the program it names, started for this store (its arguments
# name this store's data): after a reboot the number can belong to anything, and on this Mac the author's store, CI's
# and the rehearsal's all run programs named mariadbd and qdrant (REDTEAM-023).
alive() { # alive PIDFILE PROGRAM MARKER
    local p
    [ -s "$1" ] || return 1
    p="$(cat "$1")"
    [[ "$p" =~ ^[0-9]+$ ]] && kill -0 "$p" 2>/dev/null || return 1
    [ "$(basename "$(ps -p "$p" -o comm= 2>/dev/null)")" = "$2" ] || return 1
    ps -p "$p" -o args= 2>/dev/null | grep -qF -- "$3"
}
m_alive() { alive "$mpid" mariadbd "--datadir=$mdata/data"; }
q_alive() { alive "$qpid" qdrant "--config-path $qdata/config.yaml"; }
port_taken() { lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1; }
root_env() { MYSQL_PWD="$(cat "$SCRIBODO_SECRETS/db_root_password")" "$@"; }

# detach DIR LOG PIDFILE PROGRAM ARGS...: the program runs in a session of its own (a signal to the caller's process
# group, such as the app's supervisor cancelling `up`, never reaches it), in DIR, with stdin from /dev/null, output
# to LOG, and no other inherited descriptor: the app spawns this script with its own sockets open, and a daemon must
# not keep them (REDTEAM-023). The daemon's pid goes to PIDFILE.
detach() {
    python3 -I - "$@" <<'PY'
import os, sys
d, log, pidfile, argv = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
pid = os.fork()
if pid == 0:
    os.setsid()
    os.chdir(d)
    null = os.open("/dev/null", os.O_RDONLY)
    out = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    os.dup2(null, 0); os.dup2(out, 1); os.dup2(out, 2)
    os.closerange(3, max(4, min(os.sysconf("SC_OPEN_MAX"), 1 << 20)))
    os.execv(argv[0], argv)
with open(pidfile, "w") as f:
    f.write("%d\n" % pid)
PY
}

native_need() { # returns 3 (MariaDB) or 4 (Qdrant), the exit codes above
    local missing=0
    [ -x "$QDRANT_BIN" ] || { echo "Qdrant isn't built: make qdrant (the pinned 1.20.0 release binary; or set SCRIBODO_QDRANT_BIN)" >&2; missing=4; }
    [ -x "$MARIADB_BIN/mariadbd" ] || { echo "MariaDB 11.4 isn't installed: brew install mariadb@11.4 (or set SCRIBODO_MARIADB_BIN)" >&2; missing=3; }
    [ "$missing" = 0 ] || return "$missing"
    # The release the container image is pinned to, or a newer patch of its series with a warning that `up` gives last
    # (AUDIT-004 F13): another series or an older patch is refused by name.
    mariadb_version_check "$MARIADB_BIN/mariadbd" || return 3
}

mariadbd_args() {
    local flags
    read -r -a flags <<< "$SCRIBODO_MARIADB_SERVER_FLAGS"   # tools/services.defaults: the container's settings too
    printf '%s\n' --no-defaults --datadir="$mdata/data" --socket="$sock" --pid-file="$mpid" \
        --log-error="$SCRIBODO_LOGS/mariadb.log" "${flags[@]}"
}

start_mariadbd() { # start_mariadbd EXTRA...: wait for the socket
    local args=()
    while IFS= read -r a; do args+=("$a"); done < <(mariadbd_args)
    rm -f "$sock"
    detach "$mdata" "$SCRIBODO_LOGS/mariadb.log" "$mdata/launch.pid" "$MARIADB_BIN/mariadbd" "${args[@]}" "$@" || return 1
    for _ in $(seq 1 120); do [ -S "$sock" ] && m_alive && return 0; sleep 0.5; done
    echo "MariaDB did not start; see $SCRIBODO_LOGS/mariadb.log" >&2; return 1
}

stop_mariadbd() { # stop_mariadbd: true once it is gone
    m_alive || return 0
    root_env mdb mariadb-admin -uroot --socket="$sock" shutdown 2>/dev/null || kill "$(cat "$mpid")" 2>/dev/null || true
    for _ in $(seq 1 120); do m_alive || return 0; sleep 0.5; done
    echo "MariaDB did not stop within a minute (pid $(cat "$mpid")); nothing was removed" >&2; return 1
}

# First use of a data directory: initialised, then set up on a server that listens on the private socket only, so the
# accounts are never reachable over TCP before they have passwords. The marker is written only once the setup has
# succeeded, so an interrupted first start is finished by the next `up` instead of leaving root without a password
# (REDTEAM-023). The setup tries root without a password (a fresh directory) and then with the generated one (a
# setup interrupted after the ALTER).
native_init() {
    # Every step is checked here rather than left to `set -e`, which bash ignores inside a function called on the left
    # of `||`, as migrate-from-docker calls native_up (REDTEAM-023).
    if [ ! -d "$mdata/data/mysql" ]; then
        # Installed into a fresh directory and renamed into place: an install that dies part-way leaves no data
        # directory to be mistaken for a finished one.
        rm -rf "$mdata/data.installing"
        mdb mariadb-install-db --datadir="$mdata/data.installing" --auth-root-authentication-method=normal >"$mdata/install.log" 2>&1 ||
            { echo "mariadb-install-db failed; see $mdata/install.log" >&2; return 1; }
        # A data directory without the system tables is not a store (an install interrupted under older code): it
        # would swallow the new one as a subdirectory.
        [ ! -d "$mdata/data" ] || rm -rf "${mdata:?}/data" || return 1
        mv "$mdata/data.installing" "$mdata/data" || return 1
    fi
    [ -f "$mdata/initialized" ] && return 0
    stop_mariadbd || return 1
    start_mariadbd --skip-networking || return 1
    local rp up setup
    rp="$(cat "$SCRIBODO_SECRETS/db_root_password")"; up="$(cat "$SCRIBODO_SECRETS/db_password")"
    setup="ALTER USER 'root'@'localhost' IDENTIFIED BY '$rp';
ALTER USER IF EXISTS 'root'@'127.0.0.1' IDENTIFIED BY '$rp';
ALTER USER IF EXISTS 'root'@'::1' IDENTIFIED BY '$rp';
CREATE DATABASE IF NOT EXISTS scribodo CHARACTER SET utf8mb4;
CREATE USER IF NOT EXISTS 'scribodo'@'%' IDENTIFIED BY '$up';
ALTER USER 'scribodo'@'%' IDENTIFIED BY '$up';
GRANT ALL ON scribodo.* TO 'scribodo'@'%';
FLUSH PRIVILEGES;"
    if ! printf '%s\n' "$setup" | mdb mariadb -uroot --socket="$sock" --skip-ssl-verify-server-cert 2>/dev/null &&
       ! printf '%s\n' "$setup" | root_env mdb mariadb -uroot --socket="$sock" --skip-ssl-verify-server-cert; then
        stop_mariadbd || true
        echo "the first-time setup of MariaDB failed; see $SCRIBODO_LOGS/mariadb.log" >&2; return 1
    fi
    : > "$mdata/initialized" || return 1
    stop_mariadbd
}

native_up() {
    native_need || return $?
    # Credentials that existed before this call belong to a store; with no native store beside them and no remembered
    # native mode (a purged native store remembers it), that store is the containers'. Starting an empty native store
    # over it would show the author an empty project store and offer to re-import stale files (REDTEAM-023): migrate
    # instead, or say an empty store is wanted. Every step below is checked (see native_init).
    if ! native_store_exists && [ "$had_credentials" = 1 ] && [ "${SCRIBODO_NATIVE_FRESH:-}" != 1 ] &&
       [ "$(cat "$SCRIBODO_SECRETS/mode" 2>/dev/null)" != native ]; then
        echo "these credentials belong to an existing store with no native copy: tools/services.sh migrate-from-docker moves it (SCRIBODO_NATIVE_FRESH=1 starts an empty native store instead)" >&2
        return 2
    fi
    secrets native || return 1
    mkdir -p "$mdata" "$qdata/storage" "$qdata/snapshots" "$SCRIBODO_LOGS" && chmod 700 "$mdata" "$qdata" || return 1
    native_init || return 1
    if ! m_alive; then
        if port_taken "$SCRIBODO_DB_PORT"; then
            echo "port $SCRIBODO_DB_PORT is taken (the Docker containers? SCRIBODO_SERVICES=docker $0 down)" >&2; return 5
        fi
        start_mariadbd --port="$SCRIBODO_DB_PORT" --bind-address=127.0.0.1 || return 1
    fi
    for _ in $(seq 1 60); do root_env mdb mariadb-admin -uroot --socket="$sock" ping >/dev/null 2>&1 && break; sleep 0.5; done
    root_env mdb mariadb-admin -uroot --socket="$sock" ping >/dev/null 2>&1 || { echo "MariaDB is up but refuses the root password; see $SCRIBODO_LOGS/mariadb.log" >&2; return 1; }
    # What mariadb-install-db creates beyond what the container has, removed on every up so an older data directory
    # is hardened too (REDTEAM-023): anonymous accounts, root at the machine's host name (no password), the test
    # database and the PUBLIC grants on test databases; and the grants of dropped test databases. Also any one-time
    # restore account (scribodo_restore_<pid>, full rights on its database, a password nobody kept) that a load left
    # behind when the server died under it (verification of AUDIT-004, 2026-11-16).
    root_env mdb mariadb -uroot --socket="$sock" --skip-ssl-verify-server-cert <<'SQL' || { echo "hardening the accounts failed" >&2; return 1; }
DELETE FROM mysql.global_priv WHERE User = '' OR (User = 'root' AND Host NOT IN ('localhost', '127.0.0.1', '::1'));
DELETE FROM mysql.db WHERE Db LIKE 'test%';
DROP DATABASE IF EXISTS test;
DELETE FROM mysql.db WHERE (Db LIKE 'scribodo\_test\_%' OR Db LIKE 'scribodo\_crash\_%')
    AND Db NOT IN (SELECT SCHEMA_NAME FROM information_schema.SCHEMATA);
DELETE FROM mysql.global_priv WHERE User LIKE 'scribodo\_restore\_%';
DELETE FROM mysql.db WHERE User LIKE 'scribodo\_restore\_%';
FLUSH PRIVILEGES;
SQL
    # Qdrant: loopback, HTTP only, the API key by environment, telemetry off, storage and working directory beside the
    # secrets.
    if ! q_alive; then
        if port_taken "$SCRIBODO_QDRANT_PORT"; then
            echo "port $SCRIBODO_QDRANT_PORT is taken (the Docker containers? SCRIBODO_SERVICES=docker $0 down)" >&2; return 5
        fi
        cat > "$qdata/config.yaml" <<EOF || return 1
log_level: INFO
storage:
  storage_path: $qdata/storage
  snapshots_path: $qdata/snapshots
service:
  host: 127.0.0.1
  http_port: $SCRIBODO_QDRANT_PORT
  grpc_port: null
  enable_cors: false
telemetry_disabled: true
EOF
        QDRANT__SERVICE__API_KEY="$(cat "$SCRIBODO_SECRETS/qdrant_api_key")" \
            detach "$qdata" "$SCRIBODO_LOGS/qdrant.log" "$qpid" "$QDRANT_BIN" --config-path "$qdata/config.yaml" || return 1
    fi
    for _ in $(seq 1 120); do
        [ "$(qdrant_curl -o /dev/null -w '%{http_code}' "http://127.0.0.1:$SCRIBODO_QDRANT_PORT/collections")" = 200 ] && break
        sleep 0.5
    done
    [ "$(qdrant_curl -o /dev/null -w '%{http_code}' "http://127.0.0.1:$SCRIBODO_QDRANT_PORT/collections")" = 200 ] ||
        { echo "Qdrant did not start; see $SCRIBODO_LOGS/qdrant.log" >&2; return 1; }
}

native_down() { # true once both are gone
    stop_mariadbd || return 1
    if q_alive; then kill "$(cat "$qpid")" 2>/dev/null || true; fi
    for _ in $(seq 1 60); do q_alive || break; sleep 0.5; done
    if q_alive; then echo "Qdrant did not stop (pid $(cat "$qpid"))" >&2; return 1; fi
    rm -f "$qpid"
}

native_status() {
    local m="stopped" q="stopped"
    m_alive && m="running (pid $(cat "$mpid"), 127.0.0.1:$SCRIBODO_DB_PORT)"
    q_alive && q="running (pid $(cat "$qpid"), 127.0.0.1:$SCRIBODO_QDRANT_PORT)"
    printf 'mode\tnative\nmariadb\t%s\nqdrant\t%s\n' "$m" "$q"
}

# The author's own store needs saying so before anything is stopped or removed, however its path is spelled
# (REDTEAM-023: the guard was a string comparison that ran after the services had already been stopped).
purge_guard() {
    if is_default_store && [ "${SCRIBODO_PURGE_CONFIRM:-}" != 1 ]; then
        echo "this would delete the author's store under $SCRIBODO_SECRETS; set SCRIBODO_PURGE_CONFIRM=1 to do it" >&2; leave 2
    fi
}

native_purge() {
    purge_guard
    native_down || { echo "the services are still running; nothing was deleted" >&2; leave 1; }
    rm -rf "${mdata:?}" "${qdata:?}"
    rm -f "$sock"
    # The credentials stay (an older container volume may still need them); the remembered mode says native, so the
    # next `up` starts a fresh native store instead of reading the folder as a Docker user's (REDTEAM-023), unless the
    # mode was forced for this call, which is never remembered.
    remember_mode native
}

# The app writes to the author's own store; a throwaway store (CI, the rehearsal) is not the app's.
app_running() { is_default_store && pgrep -x scribodo >/dev/null 2>&1; }

# migrate-from-docker (AUDIT-004 F6: these were 115 lines inside the `case`). Its state (prior, native_existed,
# native_was_up, containers_were_up, keepn) is global, for migration_restore and migration_rollback; it reads `mode`,
# set before the commands run. Called as a plain command, so `set -e` holds inside it as it did inline.
migrate_from_docker() {
    # The author's store moves from the containers to the native services. Refused while the app runs (its writes
    # after the snapshot would stay behind) and when the native store already holds data (it would be replaced;
    # SCRIBODO_MIGRATE_REPLACE=1 dumps it beside the secrets first). The mode becomes native only after the load and
    # its check succeed. Any failure puts both stores back exactly as they were before the migration began: the
    # containers up or down as they were, the native services up or down as they were, a native data directory this
    # migration created removed, a replaced native store reloaded from its dump (REDTEAM-023).
    [ -z "${SCRIBODO_SERVICES:-}" ] || { echo "unset SCRIBODO_SERVICES for the migration" >&2; leave 2; }
    app_running && { echo "quit Scribodo first (it would keep writing to the store being moved)" >&2; leave 2; }
    native_need || leave $?
    docker info >/dev/null 2>&1 || { echo "Docker isn't running; the store to move is in its containers" >&2; leave 6; }
    prior="$mode"
    native_existed=0; native_store_exists && native_existed=1
    native_was_up=0; m_alive && native_was_up=1
    if [ "$native_existed" = 1 ] && [ "${SCRIBODO_MIGRATE_REPLACE:-}" != 1 ]; then
        echo "a native store already exists; nothing was changed (SCRIBODO_MIGRATE_REPLACE=1 keeps a dump of it and replaces it)" >&2; leave 2
    fi
    mkdir -p "$SCRIBODO_SECRETS" && chmod 700 "$SCRIBODO_SECRETS"
    # The choice is written down now, so a native data directory a failed migration leaves can never become the
    # store by default.
    [ -s "$SCRIBODO_SECRETS/mode" ] || write_mode "$prior" || { echo "the mode could not be written; nothing was changed" >&2; leave 1; }
    export SCRIBODO_SERVICES=docker
    containers_were_up=0; [ -n "$(compose ps -q 2>/dev/null)" ] && containers_were_up=1
    keepn=""
    # The two stores share their ports: the native services stop before the containers start.
    if [ "$native_was_up" = 1 ]; then native_down || { echo "the native services did not stop; nothing was changed" >&2; leave 1; }; fi
    secrets docker
    compose up -d --wait --wait-timeout 180 >/dev/null || migration_rollback
    # The containers' MariaDB keeps the root password its volume was initialised with; the secrets folder may hold a
    # newer one (credentials rotated or regenerated after the volume was made). Said by name before anything moves,
    # rather than as the dump's "Access denied".
    if ! err="$(rootsql -N -e 'SELECT 1' 2>&1 >/dev/null)"; then
        case "$err" in
            *"Access denied"*) echo "the containers' root password does not match the secrets folder (their volume was initialised under other credentials); nothing moved" >&2 ;;
            *) printf 'the containers'"'"' MariaDB does not answer: %s\n' "$err" >&2 ;;
        esac
        migration_rollback
    fi
    stamp="$(date +%Y%m%d-%H%M%S)"
    keep="$SCRIBODO_SECRETS/migration-$stamp.sql.gz"
    umask 077
    room_for_dump "the containers' dump" || migration_rollback
    dump_db scribodo | gzip -9 > "$keep.part" || { rm -f "$keep.part"; migration_rollback; }
    gzip -dc "$keep.part" | tail -1 | grep -q '^-- Dump completed' || { echo "the dump is incomplete; nothing moved" >&2; rm -f "$keep.part"; migration_rollback; }
    mv "$keep.part" "$keep" || migration_rollback
    say "dumped the containers' scribodo database to $keep"
    compose down || migration_rollback
    export SCRIBODO_SERVICES=native
    SCRIBODO_NATIVE_FRESH=1 native_up >/dev/null || migration_rollback
    tables="$(rootsql -N -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'scribodo'")" || migration_rollback
    if [ "$tables" != 0 ] && [ "${SCRIBODO_MIGRATE_REPLACE:-}" != 1 ]; then
        echo "the native store already holds $tables tables; nothing was replaced (SCRIBODO_MIGRATE_REPLACE=1 keeps a dump of it and replaces it)" >&2
        migration_restore || true; leave 2
    fi
    # A native store that existed before the migration is dumped before the load, even with no table in it: a failed
    # load is undone by reloading this dump into an emptied database, so the store is exactly as it was (verification
    # of AUDIT-004, 2026-11-16: an empty store was skipped and kept the containers' half-loaded tables). A store this
    # migration created is removed whole instead.
    if [ "$native_existed" = 1 ] || [ "$tables" != 0 ]; then
        # The dump is trusted for a reload only once it is complete (REDTEAM-023: a part-written one would have been
        # loaded over the intact native store).
        native_dump="$SCRIBODO_SECRETS/migration-native-$stamp.sql.gz"
        room_for_dump "the native store's dump" || migration_rollback
        { dump_db scribodo | gzip -9 > "$native_dump.part"; } || { rm -f "$native_dump.part"; migration_rollback; }
        gzip -dc "$native_dump.part" | tail -1 | grep -q '^-- Dump completed' || { echo "the native store's dump is incomplete; nothing moved" >&2; rm -f "$native_dump.part"; migration_rollback; }
        mv "$native_dump.part" "$native_dump" || migration_rollback
        keepn="$native_dump"
        say "kept the native store's previous contents in $keepn"
    fi
    gzip -dc "$keep" | load_dump scribodo || migration_rollback
    v="$(rootsql -N -e 'SELECT MAX(version) FROM schema_migrations' scribodo)" || migration_rollback
    [ -n "$v" ] && [ "$v" != NULL ] || migration_rollback
    write_mode native || migration_rollback
    # From here the migration has happened: what follows only reports it, and a closed stdout must not undo it.
    say "loaded into the native MariaDB: schema version $v; the semantic index is rebuilt by the app (Rebuild memory, the observer's index); the containers' volume is kept"
    # The dumps accumulate (one per attempt, two with SCRIBODO_MIGRATE_REPLACE=1): say where they are.
    local dumps
    dumps="$(find "$SCRIBODO_SECRETS" -maxdepth 1 -name 'migration-*.sql.gz' 2>/dev/null | wc -l | tr -d ' ')" || dumps="?"
    say "the migration dumps are kept in $SCRIBODO_SECRETS ($dumps migration-*.sql.gz files, $(du -ch "$SCRIBODO_SECRETS"/migration-*.sql.gz 2>/dev/null | tail -1 | cut -f1)); remove the ones no longer needed"
    ( native_status ) 2>/dev/null || true
}

# room_for_dump WHAT: true when the secrets folder's volume has room for a dump of the store the current mode reaches;
# otherwise says so. The bound is the database's size uncompressed (the dump is gzip'd, so smaller). A test may state
# the free space (SCRIBODO_MIGRATE_ASSUME_FREE_KB) instead of filling a disk.
room_for_dump() {
    local need free
    need="$(rootsql -N -e "SELECT COALESCE(SUM(data_length + index_length), 0) DIV 1024 + 1024 FROM information_schema.tables WHERE table_schema = 'scribodo'")" || return 1
    free="${SCRIBODO_MIGRATE_ASSUME_FREE_KB:-$(df -Pk "$SCRIBODO_SECRETS" | awk 'NR == 2 { print $4 }')}"
    [[ "$need" =~ ^[0-9]+$ ]] || { echo "the size of the store could not be read before $1; nothing moved" >&2; return 1; }
    [[ "$free" =~ ^[0-9]+$ ]] || free=0
    if [ "$free" -lt "$need" ]; then
        echo "not enough free space beside the secrets for $1 (about $((need / 1024)) MB needed, $((free / 1024)) MB free in $SCRIBODO_SECRETS); nothing moved" >&2
        return 1
    fi
}

# migration_restore: true when both stores are back as they were. Every step is guarded: it runs under `set -e`. The two
# stores share their ports, so the order matters: the native store is reloaded and stopped, then the containers this
# migration started are stopped, and only then do the native services that were running come back (verification of
# AUDIT-004, 2026-11-16: restarting them first found the containers' port taken and left the author's store down).
migration_restore() {
    local clean=0
    export SCRIBODO_SERVICES=native
    if [ -n "$keepn" ]; then
        # The load reached a store that existed: it goes back to its dump, whatever happened to the server meanwhile.
        # A MariaDB that died during the load is started again for the reload (the containers are already down, so
        # their ports are free); one that cannot be started is said, never skipped (verification of AUDIT-004).
        if ! m_alive; then SCRIBODO_NATIVE_FRESH=1 native_up >/dev/null || true; fi
        if ! m_alive; then
            echo "the native MariaDB is not running, so its previous contents were not reloaded; they are kept in $keepn" >&2; clean=1
        # The database is emptied first, so a table only the failed load created does not survive the reload.
        elif [ -s "$keepn" ] &&
             printf "DROP DATABASE IF EXISTS scribodo;\nCREATE DATABASE scribodo CHARACTER SET utf8mb4;\nGRANT ALL ON scribodo.* TO 'scribodo'@'%%';\n" | rootsql &&
             gzip -dc "$keepn" | load_dump scribodo; then :
        else
            echo "the native store's previous contents could not be reloaded; they are kept in $keepn" >&2; clean=1
        fi
        # A load the server died under leaves its one-time account (full rights, a password nobody kept).
        if m_alive; then
            printf "DELETE FROM mysql.global_priv WHERE User LIKE 'scribodo\\_restore\\_%%';\nDELETE FROM mysql.db WHERE User LIKE 'scribodo\\_restore\\_%%';\nFLUSH PRIVILEGES;\n" |
                rootsql || { echo "a one-time restore account could not be removed; tools/services.sh up removes it" >&2; clean=1; }
        fi
    fi
    if native_down; then
        if [ "$native_existed" = 0 ]; then rm -rf "${mdata:?}" "${qdata:?}" || { echo "the native directories this migration created could not be removed" >&2; clean=1; }; fi
    else
        echo "the native services did not stop; their directories were left in place" >&2; clean=1
    fi
    export SCRIBODO_SERVICES=docker
    if [ "$containers_were_up" = 0 ]; then
        compose down >/dev/null 2>&1 ||
            { echo "the containers this migration started could not be stopped: SCRIBODO_SERVICES=docker tools/services.sh down" >&2; clean=1; }
    fi
    export SCRIBODO_SERVICES=native
    if [ "$native_was_up" = 1 ] && ! SCRIBODO_NATIVE_FRESH=1 native_up >/dev/null; then
        # Say what is running: native_up can leave MariaDB up when Qdrant's port is taken.
        local mstate=stopped qstate=stopped
        m_alive && mstate=running
        q_alive && qstate=running
        if [ "$mstate$qstate" = stoppedstopped ]; then echo "the native services did not come back: tools/services.sh up" >&2
        else echo "the native services came back only in part: MariaDB $mstate, Qdrant $qstate (tools/services.sh up)" >&2; fi
        clean=1
    fi
    export SCRIBODO_SERVICES=docker
    if [ "$containers_were_up" = 1 ]; then
        { secrets docker && compose up -d --wait --wait-timeout 180 >/dev/null; } ||
            { echo "the containers did not come back: SCRIBODO_SERVICES=docker tools/services.sh up" >&2; clean=1; }
    fi
    return $clean
}

migration_rollback() {
    if migration_restore; then echo "the migration failed; both stores are as they were before it and the remembered mode is unchanged" >&2
    else echo "the migration failed, and putting the stores back was incomplete (above); the remembered mode is unchanged" >&2; fi
    leave 1
}

# ---- the commands ----------------------------------------------------------------------------------------------------
mode="$(services_mode)"
case "${1:-status}" in
    mode) echo "$mode" ;;
    up)
        if [ "$mode" = native ]; then
            native_up || leave $?
            remember_mode native
            native_status
            # Last on stderr, so the app shows it (the `warning: ` line in the exit-code contract above).
            [ -z "${MARIADB_VERSION_WARNING:-}" ] || echo "warning: $MARIADB_VERSION_WARNING" >&2
        else
            docker info >/dev/null 2>&1 || { echo "Docker isn't running (start Docker Desktop, or install the native services: brew install mariadb@11.4 && make qdrant, then tools/services.sh migrate-from-docker)" >&2; leave 6; }
            secrets docker
            compose up -d --wait --wait-timeout 180
            remember_mode docker
            compose ps --format 'table {{.Service}}\t{{.Status}}\t{{.Ports}}'
        fi ;;
    testdb) # testdb create|drop|sql NAME: a throwaway database for tests (sql: run stdin against it, read-only diagnosis)
        [ -n "${3:-}" ] && { [ "${2:-}" = create ] || [ "${2:-}" = drop ] || [ "${2:-}" = sql ]; } || usage
        name="$3"
        # Strictly a test name: no quotes, backticks or separators can reach SQL.
        if ! [[ "$name" =~ ^scribodo_test_[a-z0-9_]+$ ]]; then
            echo "test databases are named scribodo_test_[a-z0-9_]+" >&2; leave 2
        fi
        if [ "${2:-}" = sql ]; then
            rootsql --batch "$name"
            leave 0
        fi
        if [ "${2:-}" = create ]; then
            printf 'CREATE DATABASE IF NOT EXISTS `%s` CHARACTER SET utf8mb4;\nGRANT ALL ON `%s`.* TO '"'"'scribodo'"'"'@'"'"'%%'"'"';\n' "$name" "$name" | rootsql
        else
            # and its grant, which DROP DATABASE leaves behind
            printf 'DROP DATABASE IF EXISTS `%s`;\nDELETE FROM mysql.db WHERE Db = '"'"'%s'"'"';\nFLUSH PRIVILEGES;\n' "$name" "$name" | rootsql
            # and its semantic index collections ("<database>__<project>")
            qdrant_delete_collections "$name"
        fi
        ;;
    rootsql) shift; rootsql "$@" ;;   # stdin to the client as root (backup, restore and their tests)
    dump) [ -n "${2:-}" ] || usage; dump_db "$2" ;;
    rotate-root) # new MariaDB root password, set without ever appearing in ps
        f="$SCRIBODO_SECRETS/db_root_password"
        new="$(openssl rand -hex 24)"
        if printf "ALTER USER IF EXISTS 'root'@'%%' IDENTIFIED BY '%s'; ALTER USER IF EXISTS 'root'@'localhost' IDENTIFIED BY '%s'; ALTER USER IF EXISTS 'root'@'127.0.0.1' IDENTIFIED BY '%s'; ALTER USER IF EXISTS 'root'@'::1' IDENTIFIED BY '%s';\n" "$new" "$new" "$new" "$new" | rootsql; then
            # Rewrite in place: the container's bind mount follows the inode.
            printf '%s' "$new" > "$f" && echo "rotated"
        else
            echo "rotation failed; the old password is unchanged" >&2; leave 1
        fi
        ;;
    migrate-from-docker) migrate_from_docker ;;
    down) if [ "$mode" = native ]; then native_down; else compose down; fi ;;
    purge)
        if [ "$mode" = native ]; then native_purge; else
            [ "${SCRIBODO_COMPOSE_PROJECT:-scribodo}" != scribodo ] || [ "${SCRIBODO_PURGE_CONFIRM:-}" = 1 ] ||
                { echo "this would delete the author's container volumes; set SCRIBODO_PURGE_CONFIRM=1 to do it" >&2; leave 2; }
            compose down -v --remove-orphans
        fi ;;
    status) if [ "$mode" = native ]; then native_status; else compose ps --format 'table {{.Service}}\t{{.Status}}\t{{.Ports}}'; fi ;;
    logs) if [ "$mode" = native ]; then tail -n 50 "$SCRIBODO_LOGS/mariadb.log" "$SCRIBODO_LOGS/qdrant.log" 2>/dev/null; else compose logs --tail 50; fi ;;
    *) usage ;;
esac
# The end of a command that ran to completion. `set -e` does not apply to every command (the left of `&&` or `||`), so a
# non-zero status here is still a failure: never 2 or a contract code by accident.
rc=$?
[ "$rc" = 0 ] || rc=1
leave "$rc"
