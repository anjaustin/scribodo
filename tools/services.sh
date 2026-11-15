#!/bin/bash
# tools/services.sh up|down|status|logs|purge|mode|testdb create|drop|sql NAME|rootsql [args]|dump DB|rotate-root|migrate-from-docker
#
# Starts Scribodo's local services (MariaDB 11.4, Qdrant 1.20) natively on this Mac or in Docker Compose
# (tools/services_lib.sh says how the mode is chosen; SCRIBODO_SERVICES=native|docker forces one for this call without
# changing the remembered mode). Credentials are generated once into a 0700 folder in Application Support and never
# printed. Everything binds to 127.0.0.1.
set -euo pipefail
. "$(dirname "$0")/services_lib.sh"

# One mutating command at a time per store: the app's supervisor and a terminal can both run `up` (REDTEAM-023).
case "${1:-status}" in
    up|down|purge|migrate-from-docker|rotate-root)
        if [ -z "${SCRIBODO_SERVICES_LOCKED:-}" ]; then
            mkdir -p "$SCRIBODO_SECRETS" && chmod 700 "$SCRIBODO_SECRETS"
            SCRIBODO_SERVICES_LOCKED=1 exec /usr/bin/lockf -k -t 300 "$SCRIBODO_SECRETS/.services.lock" "$0" "$@"
        fi ;;
esac

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
remember_mode() { [ -n "${SCRIBODO_SERVICES:-}" ] || printf '%s\n' "$1" > "$SCRIBODO_SECRETS/mode"; }

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

native_need() {
    local missing=0
    [ -x "$MARIADB_BIN/mariadbd" ] || { echo "MariaDB 11.4 isn't installed: brew install mariadb@11.4 (or set SCRIBODO_MARIADB_BIN)" >&2; missing=1; }
    [ -x "$QDRANT_BIN" ] || { echo "Qdrant isn't built: make qdrant (the pinned 1.20.0 release binary; or set SCRIBODO_QDRANT_BIN)" >&2; missing=1; }
    [ "$missing" = 0 ] || return 1
    # Homebrew's formula is not pinned the way the container image is: the series is (REDTEAM-023), and `brew upgrade`
    # within 11.4 is a patch release.
    "$MARIADB_BIN/mariadbd" --no-defaults --version 2>/dev/null | grep -q " 11\.4\." ||
        { echo "MariaDB at $MARIADB_BIN is not the 11.4 series the store is built and tested on" >&2; return 1; }
}

mariadbd_args() {
    printf '%s\n' --no-defaults --datadir="$mdata/data" --socket="$sock" --pid-file="$mpid" \
        --log-error="$SCRIBODO_LOGS/mariadb.log" --character-set-server=utf8mb4 --collation-server=utf8mb4_uca1400_ai_ci \
        --innodb-buffer-pool-size=128M --max-connections=32 --skip-name-resolve
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
    native_need || return 1
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
            echo "port $SCRIBODO_DB_PORT is taken (the Docker containers? SCRIBODO_SERVICES=docker $0 down)" >&2; return 1
        fi
        start_mariadbd --port="$SCRIBODO_DB_PORT" --bind-address=127.0.0.1 || return 1
    fi
    for _ in $(seq 1 60); do root_env mdb mariadb-admin -uroot --socket="$sock" ping >/dev/null 2>&1 && break; sleep 0.5; done
    root_env mdb mariadb-admin -uroot --socket="$sock" ping >/dev/null 2>&1 || { echo "MariaDB is up but refuses the root password; see $SCRIBODO_LOGS/mariadb.log" >&2; return 1; }
    # What mariadb-install-db creates beyond what the container has, removed on every up so an older data directory
    # is hardened too (REDTEAM-023): anonymous accounts, root at the machine's host name (no password), the test
    # database and the PUBLIC grants on test databases; and the grants of dropped test databases.
    root_env mdb mariadb -uroot --socket="$sock" --skip-ssl-verify-server-cert <<'SQL' || { echo "hardening the accounts failed" >&2; return 1; }
DELETE FROM mysql.global_priv WHERE User = '' OR (User = 'root' AND Host NOT IN ('localhost', '127.0.0.1', '::1'));
DELETE FROM mysql.db WHERE Db LIKE 'test%';
DROP DATABASE IF EXISTS test;
DELETE FROM mysql.db WHERE (Db LIKE 'scribodo\_test\_%' OR Db LIKE 'scribodo\_crash\_%')
    AND Db NOT IN (SELECT SCHEMA_NAME FROM information_schema.SCHEMATA);
FLUSH PRIVILEGES;
SQL
    # Qdrant: loopback, HTTP only, the API key by environment, telemetry off, storage and working directory beside the
    # secrets.
    if ! q_alive; then
        if port_taken "$SCRIBODO_QDRANT_PORT"; then
            echo "port $SCRIBODO_QDRANT_PORT is taken (the Docker containers? SCRIBODO_SERVICES=docker $0 down)" >&2; return 1
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
    local key; key="$(cat "$SCRIBODO_SECRETS/qdrant_api_key")"
    for _ in $(seq 1 120); do
        [ "$(curl -s -o /dev/null -w '%{http_code}' -H @<(printf 'api-key: %s\n' "$key") "http://127.0.0.1:$SCRIBODO_QDRANT_PORT/collections")" = 200 ] && break
        sleep 0.5
    done
    [ "$(curl -s -o /dev/null -w '%{http_code}' -H @<(printf 'api-key: %s\n' "$key") "http://127.0.0.1:$SCRIBODO_QDRANT_PORT/collections")" = 200 ] ||
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
        echo "this would delete the author's store under $SCRIBODO_SECRETS; set SCRIBODO_PURGE_CONFIRM=1 to do it" >&2; exit 2
    fi
}

native_purge() {
    purge_guard
    native_down || { echo "the services are still running; nothing was deleted" >&2; exit 1; }
    rm -rf "${mdata:?}" "${qdata:?}"
    rm -f "$sock"
    # The credentials stay (an older container volume may still need them); the remembered mode says native, so the
    # next `up` starts a fresh native store instead of reading the folder as a Docker user's (REDTEAM-023), unless the
    # mode was forced for this call, which is never remembered.
    remember_mode native
}

# The app writes to the author's own store; a throwaway store (CI, the rehearsal) is not the app's.
app_running() { is_default_store && pgrep -x scribodo >/dev/null 2>&1; }

# ---- the commands ----------------------------------------------------------------------------------------------------
mode="$(services_mode)"
case "${1:-status}" in
    mode) echo "$mode" ;;
    up)
        if [ "$mode" = native ]; then
            native_up || exit $?
            remember_mode native
            native_status
        else
            docker info >/dev/null 2>&1 || { echo "Docker isn't running (start Docker Desktop, or install the native services: brew install mariadb@11.4 && make qdrant, then tools/services.sh migrate-from-docker)" >&2; exit 1; }
            secrets docker
            compose up -d --wait --wait-timeout 180
            remember_mode docker
            compose ps --format 'table {{.Service}}\t{{.Status}}\t{{.Ports}}'
        fi ;;
    testdb) # testdb create|drop|sql NAME: a throwaway database for tests (sql: run stdin against it, read-only diagnosis)
        name="${3:?name}"
        # Strictly a test name: no quotes, backticks or separators can reach SQL.
        if ! [[ "$name" =~ ^scribodo_test_[a-z0-9_]+$ ]]; then
            echo "test databases are named scribodo_test_[a-z0-9_]+" >&2; exit 2
        fi
        if [ "${2:-}" = sql ]; then
            rootsql --batch "$name"
            exit $?
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
    dump) dump_db "${2:?database}" ;;
    rotate-root) # new MariaDB root password, set without ever appearing in ps
        f="$SCRIBODO_SECRETS/db_root_password"
        new="$(openssl rand -hex 24)"
        if printf "ALTER USER IF EXISTS 'root'@'%%' IDENTIFIED BY '%s'; ALTER USER IF EXISTS 'root'@'localhost' IDENTIFIED BY '%s'; ALTER USER IF EXISTS 'root'@'127.0.0.1' IDENTIFIED BY '%s'; ALTER USER IF EXISTS 'root'@'::1' IDENTIFIED BY '%s';\n" "$new" "$new" "$new" "$new" | rootsql; then
            # Rewrite in place: the container's bind mount follows the inode.
            printf '%s' "$new" > "$f" && echo "rotated"
        else
            echo "rotation failed; the old password is unchanged" >&2; exit 1
        fi
        ;;
    migrate-from-docker)
        # The author's store moves from the containers to the native services. Refused while the app runs (its writes
        # after the snapshot would stay behind) and when the native store already holds data (it would be replaced;
        # SCRIBODO_MIGRATE_REPLACE=1 dumps it beside the secrets first). The mode becomes native only after the load and
        # its check succeed. Any failure puts both stores back exactly as they were before the migration began: the
        # containers up or down as they were, the native services up or down as they were, a native data directory this
        # migration created removed, a replaced native store reloaded from its dump (REDTEAM-023).
        [ -z "${SCRIBODO_SERVICES:-}" ] || { echo "unset SCRIBODO_SERVICES for the migration" >&2; exit 2; }
        app_running && { echo "quit Scribodo first (it would keep writing to the store being moved)" >&2; exit 2; }
        native_need || exit 1
        docker info >/dev/null 2>&1 || { echo "Docker isn't running; the store to move is in its containers" >&2; exit 1; }
        prior="$mode"
        native_existed=0; native_store_exists && native_existed=1
        native_was_up=0; m_alive && native_was_up=1
        if [ "$native_existed" = 1 ] && [ "${SCRIBODO_MIGRATE_REPLACE:-}" != 1 ]; then
            echo "a native store already exists; nothing was changed (SCRIBODO_MIGRATE_REPLACE=1 keeps a dump of it and replaces it)" >&2; exit 2
        fi
        mkdir -p "$SCRIBODO_SECRETS" && chmod 700 "$SCRIBODO_SECRETS"
        # The choice is written down now, so a native data directory a failed migration leaves can never become the
        # store by default.
        [ -s "$SCRIBODO_SECRETS/mode" ] || printf '%s\n' "$prior" > "$SCRIBODO_SECRETS/mode"
        export SCRIBODO_SERVICES=docker
        containers_were_up=0; [ -n "$(compose ps -q 2>/dev/null)" ] && containers_were_up=1
        keepn=""
        # restore: true when both stores are back as they were. Every step is guarded: it runs under `set -e`.
        restore() {
            local clean=0
            export SCRIBODO_SERVICES=native
            if [ -n "$keepn" ] && [ -s "$keepn" ] && m_alive; then
                # The database is emptied first, so a table only the failed load created does not survive the reload.
                if printf "DROP DATABASE IF EXISTS scribodo;\nCREATE DATABASE scribodo CHARACTER SET utf8mb4;\nGRANT ALL ON scribodo.* TO 'scribodo'@'%%';\n" | rootsql &&
                   gzip -dc "$keepn" | load_dump scribodo; then :; else
                    echo "the native store's previous contents could not be reloaded; they are kept in $keepn" >&2; clean=1
                fi
            fi
            if native_down; then
                if [ "$native_existed" = 0 ]; then rm -rf "${mdata:?}" "${qdata:?}" || { echo "the native directories this migration created could not be removed" >&2; clean=1; }; fi
            else
                echo "the native services did not stop; their directories were left in place" >&2; clean=1
            fi
            if [ "$native_was_up" = 1 ]; then SCRIBODO_NATIVE_FRESH=1 native_up >/dev/null || { echo "the native services did not come back: tools/services.sh up" >&2; clean=1; }; fi
            export SCRIBODO_SERVICES=docker
            if [ "$containers_were_up" = 1 ]; then
                { secrets docker && compose up -d --wait --wait-timeout 180 >/dev/null; } ||
                    { echo "the containers did not come back: SCRIBODO_SERVICES=docker tools/services.sh up" >&2; clean=1; }
            else
                compose down >/dev/null 2>&1 || true
            fi
            return $clean
        }
        rollback() {
            if restore; then echo "the migration failed; both stores are as they were before it and the remembered mode is unchanged" >&2
            else echo "the migration failed, and putting the stores back was incomplete (above); the remembered mode is unchanged" >&2; fi
            exit 1
        }
        # The two stores share their ports: the native services stop before the containers start.
        if [ "$native_was_up" = 1 ]; then native_down || { echo "the native services did not stop; nothing was changed" >&2; exit 1; }; fi
        secrets docker
        compose up -d --wait --wait-timeout 180 >/dev/null || rollback
        stamp="$(date +%Y%m%d-%H%M%S)"
        keep="$SCRIBODO_SECRETS/migration-$stamp.sql.gz"
        umask 077
        dump_db scribodo | gzip -9 > "$keep.part" || { rm -f "$keep.part"; rollback; }
        gzip -dc "$keep.part" | tail -1 | grep -q '^-- Dump completed' || { echo "the dump is incomplete; nothing moved" >&2; rm -f "$keep.part"; rollback; }
        mv "$keep.part" "$keep" || rollback
        echo "dumped the containers' scribodo database to $keep"
        compose down || rollback
        export SCRIBODO_SERVICES=native
        SCRIBODO_NATIVE_FRESH=1 native_up >/dev/null || rollback
        tables="$(rootsql -N -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'scribodo'")" || rollback
        if [ "$tables" != 0 ]; then
            if [ "${SCRIBODO_MIGRATE_REPLACE:-}" != 1 ]; then
                echo "the native store already holds $tables tables; nothing was replaced (SCRIBODO_MIGRATE_REPLACE=1 keeps a dump of it and replaces it)" >&2
                restore || true; exit 2
            fi
            # The dump is trusted for a reload only once it is complete (REDTEAM-023: a part-written one would have been
            # loaded over the intact native store).
            native_dump="$SCRIBODO_SECRETS/migration-native-$stamp.sql.gz"
            { dump_db scribodo | gzip -9 > "$native_dump.part"; } || { rm -f "$native_dump.part"; rollback; }
            gzip -dc "$native_dump.part" | tail -1 | grep -q '^-- Dump completed' || { rm -f "$native_dump.part"; rollback; }
            mv "$native_dump.part" "$native_dump" || rollback
            keepn="$native_dump"
            echo "kept the native store's previous contents in $keepn"
        fi
        gzip -dc "$keep" | load_dump scribodo || rollback
        v="$(rootsql -N -e 'SELECT MAX(version) FROM schema_migrations' scribodo)" || rollback
        [ -n "$v" ] && [ "$v" != NULL ] || rollback
        printf 'native\n' > "$SCRIBODO_SECRETS/mode"
        echo "loaded into the native MariaDB: schema version $v; the semantic index is rebuilt by the app (Rebuild memory, the observer's index); the containers' volume is kept"
        native_status
        ;;
    down) if [ "$mode" = native ]; then native_down; else compose down; fi ;;
    purge)
        if [ "$mode" = native ]; then native_purge; else
            [ "${SCRIBODO_COMPOSE_PROJECT:-scribodo}" != scribodo ] || [ "${SCRIBODO_PURGE_CONFIRM:-}" = 1 ] ||
                { echo "this would delete the author's container volumes; set SCRIBODO_PURGE_CONFIRM=1 to do it" >&2; exit 2; }
            compose down -v --remove-orphans
        fi ;;
    status) if [ "$mode" = native ]; then native_status; else compose ps --format 'table {{.Service}}\t{{.Status}}\t{{.Ports}}'; fi ;;
    logs) if [ "$mode" = native ]; then tail -n 50 "$SCRIBODO_LOGS/mariadb.log" "$SCRIBODO_LOGS/qdrant.log" 2>/dev/null; else compose logs --tail 50; fi ;;
    *) echo "usage: $0 up|down|purge|status|logs|mode|testdb create|drop|sql NAME|rootsql [args]|dump DB|rotate-root|migrate-from-docker" >&2; exit 2 ;;
esac
