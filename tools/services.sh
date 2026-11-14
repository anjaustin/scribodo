#!/bin/bash
# tools/services.sh up|down|status|logs|purge|testdb create|drop|sql NAME|rootsql [args]|dump DB|rotate-root|mode|migrate-from-docker
#
# Starts Scribodo's local services (MariaDB 11.4, Qdrant 1.20) natively on this Mac or in Docker Compose
# (tools/services_lib.sh says how the mode is chosen; SCRIBODO_SERVICES=native|docker forces one). Credentials are
# generated once into a 0700 folder in Application Support and never printed. Everything binds to 127.0.0.1.
set -euo pipefail
. "$(dirname "$0")/services_lib.sh"

secrets() {
    mkdir -p "$SCRIBODO_SECRETS"
    chmod 700 "$SCRIBODO_SECRETS"
    umask 077
    gen() { openssl rand -hex 24; }
    [ -s "$SCRIBODO_SECRETS/db_password" ] || gen > "$SCRIBODO_SECRETS/db_password"
    [ -s "$SCRIBODO_SECRETS/db_root_password" ] || gen > "$SCRIBODO_SECRETS/db_root_password"
    [ -s "$SCRIBODO_SECRETS/qdrant_api_key" ] || gen > "$SCRIBODO_SECRETS/qdrant_api_key"
    printf 'QDRANT__SERVICE__API_KEY=%s\n' "$(cat "$SCRIBODO_SECRETS/qdrant_api_key")" > "$SCRIBODO_SECRETS/qdrant.env"
    if [ "$1" = docker ]; then
        # The containers read the secret files as another user: readable by the
        # Docker VM, still inside a 0700 folder on this Mac.
        chmod 644 "$SCRIBODO_SECRETS/db_password" "$SCRIBODO_SECRETS/db_root_password"
    else
        chmod 600 "$SCRIBODO_SECRETS/db_password" "$SCRIBODO_SECRETS/db_root_password"
    fi
}

# ---- native --------------------------------------------------------------------------------------------------------
mdata="$SCRIBODO_SECRETS/mariadb"
qdata="$SCRIBODO_SECRETS/qdrant"
sock="$(native_sock)"
mpid="$mdata/mariadbd.pid"
qpid="$qdata/qdrant.pid"

alive() { [ -s "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }
port_taken() { lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1; }
root_env() { MYSQL_PWD="$(cat "$SCRIBODO_SECRETS/db_root_password")" "$@"; }

native_need() {
    local missing=0
    [ -x "$MARIADB_BIN/mariadbd" ] || { echo "MariaDB 11.4 isn't installed: brew install mariadb@11.4 (or set SCRIBODO_MARIADB_BIN)" >&2; missing=1; }
    [ -x "$QDRANT_BIN" ] || { echo "Qdrant isn't built: make qdrant (the pinned 1.20.0 release binary; or set SCRIBODO_QDRANT_BIN)" >&2; missing=1; }
    [ "$missing" = 0 ]
}

native_up() {
    native_need
    secrets native
    mkdir -p "$mdata" "$qdata/storage" "$qdata/snapshots" "$SCRIBODO_LOGS"
    chmod 700 "$mdata" "$qdata"
    # MariaDB: a fresh data directory is initialised, then the root accounts take the generated password and the
    # app's user and database are created, exactly as the container's first start does.
    local fresh=0
    if [ ! -d "$mdata/data/mysql" ]; then
        "$MARIADB_BIN/mariadb-install-db" --datadir="$mdata/data" --auth-root-authentication-method=normal >"$mdata/install.log" 2>&1
        fresh=1
    fi
    if ! alive "$mpid"; then
        if port_taken "$SCRIBODO_DB_PORT"; then
            echo "port $SCRIBODO_DB_PORT is taken (the Docker containers? SCRIBODO_SERVICES=docker $0 down)" >&2; exit 1
        fi
        rm -f "$sock"
        nohup "$MARIADB_BIN/mariadbd" --datadir="$mdata/data" --port="$SCRIBODO_DB_PORT" --bind-address=127.0.0.1 \
            --socket="$sock" --pid-file="$mpid" --log-error="$SCRIBODO_LOGS/mariadb.log" \
            --character-set-server=utf8mb4 --collation-server=utf8mb4_uca1400_ai_ci \
            --innodb-buffer-pool-size=128M --max-connections=32 --skip-name-resolve >/dev/null 2>&1 &
        disown
        for _ in $(seq 1 120); do [ -S "$sock" ] && break; sleep 0.5; done
        [ -S "$sock" ] || { echo "MariaDB did not start; see $SCRIBODO_LOGS/mariadb.log" >&2; exit 1; }
    fi
    if [ "$fresh" = 1 ]; then
        local rp up
        rp="$(cat "$SCRIBODO_SECRETS/db_root_password")"; up="$(cat "$SCRIBODO_SECRETS/db_password")"
        "$MARIADB_BIN/mariadb" -uroot --socket="$sock" --skip-ssl-verify-server-cert <<EOF
ALTER USER 'root'@'localhost' IDENTIFIED BY '$rp';
ALTER USER IF EXISTS 'root'@'127.0.0.1' IDENTIFIED BY '$rp';
ALTER USER IF EXISTS 'root'@'::1' IDENTIFIED BY '$rp';
DELETE FROM mysql.global_priv WHERE User = 'root' AND Host NOT IN ('localhost', '127.0.0.1', '::1');
CREATE DATABASE IF NOT EXISTS scribodo CHARACTER SET utf8mb4;
CREATE USER IF NOT EXISTS 'scribodo'@'%' IDENTIFIED BY '$up';
GRANT ALL ON scribodo.* TO 'scribodo'@'%';
FLUSH PRIVILEGES;
EOF
    fi
    for _ in $(seq 1 60); do root_env "$MARIADB_BIN/mariadb-admin" -uroot --socket="$sock" ping >/dev/null 2>&1 && break; sleep 0.5; done
    root_env "$MARIADB_BIN/mariadb-admin" -uroot --socket="$sock" ping >/dev/null 2>&1 || { echo "MariaDB is up but refuses the root password; see $SCRIBODO_LOGS/mariadb.log" >&2; exit 1; }
    # Qdrant: loopback, HTTP only, the API key by environment, telemetry off, storage beside the secrets.
    if ! alive "$qpid"; then
        if port_taken "$SCRIBODO_QDRANT_PORT"; then
            echo "port $SCRIBODO_QDRANT_PORT is taken (the Docker containers? SCRIBODO_SERVICES=docker $0 down)" >&2; exit 1
        fi
        cat > "$qdata/config.yaml" <<EOF
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
        # Started in this shell so $! is Qdrant's own pid (a subshell's $! named the script itself and `down` then
        # left Qdrant running); the key goes by environment, never argv.
        QDRANT__SERVICE__API_KEY="$(cat "$SCRIBODO_SECRETS/qdrant_api_key")" nohup "$QDRANT_BIN" --config-path "$qdata/config.yaml" >>"$SCRIBODO_LOGS/qdrant.log" 2>&1 &
        echo $! > "$qpid"
        disown
    fi
    local key; key="$(cat "$SCRIBODO_SECRETS/qdrant_api_key")"
    for _ in $(seq 1 120); do
        [ "$(curl -s -o /dev/null -w '%{http_code}' -H @<(printf 'api-key: %s\n' "$key") "http://127.0.0.1:$SCRIBODO_QDRANT_PORT/collections")" = 200 ] && break
        sleep 0.5
    done
    [ "$(curl -s -o /dev/null -w '%{http_code}' -H @<(printf 'api-key: %s\n' "$key") "http://127.0.0.1:$SCRIBODO_QDRANT_PORT/collections")" = 200 ] ||
        { echo "Qdrant did not start; see $SCRIBODO_LOGS/qdrant.log" >&2; exit 1; }
    printf 'native\n' > "$SCRIBODO_SECRETS/mode"
    native_status
}

native_down() {
    if alive "$mpid"; then root_env "$MARIADB_BIN/mariadb-admin" -uroot --socket="$sock" shutdown 2>/dev/null || kill "$(cat "$mpid")" 2>/dev/null || true; fi
    for _ in $(seq 1 60); do alive "$mpid" || break; sleep 0.5; done
    if alive "$qpid"; then kill "$(cat "$qpid")" 2>/dev/null || true; fi
    for _ in $(seq 1 60); do alive "$qpid" || break; sleep 0.5; done
    rm -f "$qpid"
}

native_status() {
    local m="stopped" q="stopped"
    alive "$mpid" && m="running (pid $(cat "$mpid"), 127.0.0.1:$SCRIBODO_DB_PORT)"
    alive "$qpid" && q="running (pid $(cat "$qpid"), 127.0.0.1:$SCRIBODO_QDRANT_PORT)"
    printf 'mode\tnative\nmariadb\t%s\nqdrant\t%s\n' "$m" "$q"
}

native_purge() {
    native_down
    # The author's own store lives at the default location; purging it needs saying so.
    if [ "$SCRIBODO_SECRETS" = "$HOME/Library/Application Support/Scribodo/services" ] && [ "${SCRIBODO_PURGE_CONFIRM:-}" != 1 ]; then
        echo "this would delete the author's store under $SCRIBODO_SECRETS; set SCRIBODO_PURGE_CONFIRM=1 to do it" >&2; exit 2
    fi
    rm -rf "$mdata" "$qdata" "$SCRIBODO_SECRETS/mode"
    rm -f "$sock"
}

# ---- the commands ----------------------------------------------------------------------------------------------------
mode="$(services_mode)"
case "${1:-status}" in
    mode) echo "$mode" ;;
    up)
        if [ "$mode" = native ]; then native_up; else
            docker info >/dev/null 2>&1 || { echo "Docker isn't running (start Docker Desktop, or install the native services: brew install mariadb@11.4 && make qdrant)" >&2; exit 1; }
            secrets docker
            compose up -d --wait --wait-timeout 180
            printf 'docker\n' > "$SCRIBODO_SECRETS/mode"
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
            printf 'DROP DATABASE IF EXISTS `%s`;\n' "$name" | rootsql
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
    migrate-from-docker) # the author's store moves from the containers to the native services; the dump is kept
        [ "${SCRIBODO_SERVICES:-}" != docker ] || { echo "unset SCRIBODO_SERVICES for the migration" >&2; exit 2; }
        native_need
        docker info >/dev/null 2>&1 || { echo "Docker isn't running; the store to move is in its containers" >&2; exit 1; }
        SCRIBODO_SERVICES=docker compose up -d --wait --wait-timeout 180 >/dev/null
        stamp="$(date +%Y%m%d-%H%M%S)"
        keep="$SCRIBODO_SECRETS/migration-$stamp.sql.gz"
        umask 077
        SCRIBODO_SERVICES=docker dump_db scribodo | gzip -9 > "$keep.part"
        gzip -dc "$keep.part" | tail -1 | grep -q '^-- Dump completed' || { echo "the dump is incomplete; nothing moved" >&2; rm -f "$keep.part"; exit 1; }
        mv "$keep.part" "$keep"
        echo "dumped the containers' scribodo database to $keep"
        SCRIBODO_SERVICES=docker compose down
        SCRIBODO_SERVICES=native native_up >/dev/null
        gzip -dc "$keep" | SCRIBODO_SERVICES=native load_dump scribodo
        v="$(SCRIBODO_SERVICES=native rootsql -N -e 'SELECT MAX(version) FROM schema_migrations' scribodo)"
        echo "loaded into the native MariaDB: schema version $v; the semantic index is rebuilt by the app (Rebuild memory, the observer's index)"
        native_status
        ;;
    down) if [ "$mode" = native ]; then native_down; else compose down; fi ;;
    purge) if [ "$mode" = native ]; then native_purge; else compose down -v --remove-orphans; fi ;;
    status) if [ "$mode" = native ]; then native_status; else compose ps --format 'table {{.Service}}\t{{.Status}}\t{{.Ports}}'; fi ;;
    logs) if [ "$mode" = native ]; then tail -n 50 "$SCRIBODO_LOGS/mariadb.log" "$SCRIBODO_LOGS/qdrant.log" 2>/dev/null; else compose logs --tail 50; fi ;;
    *) echo "usage: $0 up|down|purge|status|logs|mode|testdb create|drop|sql NAME|rootsql [args]|dump DB|rotate-root|migrate-from-docker" >&2; exit 2 ;;
esac
