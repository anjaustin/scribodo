#!/bin/bash
# The Docker path of tools/services.sh and services_lib.sh without Docker (AUDIT-004 F6): `docker` and `curl` are
# shell functions exported to the scripts (a function wins over PATH, and survives lockf's re-exec), which record
# every call. The in-container half of the root operations really runs: the shim executes the script that
# `compose exec` would have run, with the container's secret folder and the MariaDB clients faked. The native half of
# the same operations runs against the same fake clients, so both modes are held to one transcript.
# Every store is a throwaway folder under a throwaway HOME; nothing is started.
set -u
exec </dev/null   # the fake clients read stdin: never the caller's
cd "$(dirname "$0")/.."
ROOT="$PWD"
T="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/scribodo-docker-shim.XXXXXX")" && pwd -P)"
trap 'rm -rf "$T"' EXIT
fail=0
check() { # check NAME STATUS
    if [ "$2" = 0 ]; then echo "ok   $1"; return; fi
    echo "FAIL $1"; fail=1
    sed 's/^/      docker: /' "$SHIM_LOG" | cut -c1-240; sed 's/^/      client: /' "$CLIENT_LOG"
}

mkdir -p "$T/home" "$T/run/secrets" "$T/clients" "$T/secrets"
export HOME="$T/home"
for k in $(env | sed -n 's/^\(SCRIBODO_[A-Z_]*\)=.*/\1/p'); do unset "$k"; done
export SHIM_ENV="$T/compose.env" SHIM_LOG="$T/docker.log" SHIM_RUN="$T/run/secrets" SHIM_CLIENTS="$T/clients" CLIENT_LOG="$T/client.log"
export SCRIBODO_SECRETS="$T/secrets" SCRIBODO_LOGS="$T/logs" SCRIBODO_COMPOSE_PROJECT=scribodo-test-shim
export SCRIBODO_DB_PORT=33099 SCRIBODO_QDRANT_PORT=6399
chmod 700 "$T/secrets"
printf 'rootpw-docker\n' > "$T/run/secrets/db_root_password"
printf 'rootpw-native\n' > "$T/secrets/db_root_password"
printf 'qkey-test\n' > "$T/secrets/qdrant_api_key"

# The MariaDB clients: one line per call (tool, arguments, the password they were given, their stdin).
for tool in mariadb mariadb-dump; do
    cat > "$T/clients/$tool" <<'SH'
#!/bin/sh
in="$(cat)"
printf '%s|%s|pw=%s|stdin=%s\n' "$(basename "$0")" "$*" "${MYSQL_PWD:-}" "$(printf '%s' "$in" | tr '\n' ' ')" >> "$CLIENT_LOG"
case " $* " in *" --sandbox "*) exit "${FAKE_SANDBOX_RC:-0}" ;; esac
case "$(basename "$0")" in mariadb-dump) echo "-- Dump completed" ;; esac
exit 0
SH
    chmod 755 "$T/clients/$tool"
done
# A native installation for the checks that need one (migrate-from-docker's refusals).
cat > "$T/clients/mariadbd" <<'SH'
#!/bin/sh
. "$(dirname "$0")/../version"
echo "$0  Ver $V-MariaDB for osx10.21 on arm64 (Homebrew)"
SH
chmod 755 "$T/clients/mariadbd"
(. tools/services.defaults; echo "V=$SCRIBODO_MARIADB_VERSION") > "$T/version" 2>/dev/null || echo "V=11.4.13" > "$T/version"
printf '#!/bin/sh\nexit 1\n' > "$T/qdrant"; chmod 755 "$T/qdrant"

docker() {
    { printf 'docker'; printf ' [%s]' "$@"; printf '\n'; } >> "$SHIM_LOG"
    case "$1" in info) return "${SHIM_DOCKER_INFO:-0}" ;; compose) ;; *) return 0 ;; esac
    printf '%s\n' "${SCRIBODO_MARIADB_SERVER_FLAGS:-}" > "$SHIM_ENV" # what Compose would interpolate
    shift 5 # compose -p PROJECT -f FILE
    case "$1" in
        exec) # exec -T mariadb sh -c SCRIPT sh ARGS...: the container's half, run here
            [ "$2" = -T ] && [ "$3" = mariadb ] && [ "$4" = sh ] && [ "$5" = -c ] || return 99
            local script="${6//\/run\/secrets/$SHIM_RUN}"
            shift 6
            PATH="$SHIM_CLIENTS:$PATH" sh -c "$script" "$@" ;;
        *) return 0 ;;
    esac
}
curl() { # records the URL and the header the script sent (from its -H @file)
    local a h="" url=""
    while [ $# -gt 0 ]; do
        a="$1"; shift
        case "$a" in -H) h="$(cat "${1#@}")"; shift ;; -o|-w|-X) shift ;; http*) url="$a" ;; esac
    done
    printf 'curl %s|%s\n' "$url" "$h" >> "$SHIM_LOG"
    case "$url" in */collections) echo '{"result":{"collections":[{"name":"scribodo_test_x__p1"},{"name":"other__p2"}]}}' ;; esac
}
export -f docker curl

lib() { bash -c '. tools/services_lib.sh; "$@"' lib "$@"; }
reset() { : > "$SHIM_LOG"; : > "$CLIENT_LOG"; }
compose_prefix="docker [compose] [-p] [scribodo-test-shim] [-f] [$ROOT/docker/compose.yml]"

# ---- the root operations, both modes -------------------------------------------------------------------------------
for mode in docker native; do
    export SCRIBODO_SERVICES=$mode SCRIBODO_MARIADB_BIN="$T/clients"
    if [ $mode = docker ]; then pw=rootpw-docker; conn=""; host=localhost
    else pw=rootpw-native; conn="--no-defaults -h127.0.0.1 -P33099 --skip-ssl-verify-server-cert "; host=127.0.0.1; fi

    reset
    echo 'SELECT 1;' | lib rootsql -N scribodo; r=$?
    [ "$r" = 0 ] && [ "$(cat "$CLIENT_LOG")" = "mariadb|${conn}-uroot -N scribodo|pw=$pw|stdin=SELECT 1;" ]
    check "$mode: rootsql passes its arguments and stdin, as root, the password by environment" $?
    if [ $mode = docker ]; then
        grep -qF "$compose_prefix [exec] [-T] [mariadb] [sh] [-c] [" "$SHIM_LOG" && [ "$(grep -c '^docker ' "$SHIM_LOG")" = 1 ]
        check "docker: rootsql is one compose exec in the mariadb service" $?
    else
        [ ! -s "$SHIM_LOG" ]; check "native: rootsql never calls docker" $?
    fi

    reset
    out="$(lib dump_db scribodo)"; r=$?
    [ "$r" = 0 ] && [ "$out" = "-- Dump completed" ] &&
        [ "$(cat "$CLIENT_LOG")" = "mariadb-dump|${conn}-uroot --single-transaction --quick --routines --triggers --hex-blob --default-character-set=utf8mb4 scribodo|pw=$pw|stdin=" ]
    check "$mode: dump_db is one consistent mariadb-dump as root" $?

    reset
    printf 'CREATE TABLE t (x INT);\n' | lib load_dump scribodo; r=$?
    create="$(sed -n 1p "$CLIENT_LOG")"; load="$(sed -n 2p "$CLIENT_LOG")"; drop="$(sed -n 3p "$CLIENT_LOG")"
    u="$(printf '%s' "$load" | sed -n 's/.*-u\(scribodo_restore_[0-9]*\) .*/\1/p')"
    lpw="$(printf '%s' "$load" | sed -n 's/.*|pw=\([0-9a-f]*\)|.*/\1/p')"
    [ "$r" = 0 ] && [ "$(grep -c . "$CLIENT_LOG")" = 3 ] && [ -n "$u" ] && [ "${#lpw}" = 36 ] &&
        [ "$create" = "mariadb|${conn}-uroot|pw=$pw|stdin=CREATE USER '$u'@'$host' IDENTIFIED BY '$lpw'; GRANT ALL ON \`scribodo\`.* TO '$u'@'$host';" ] &&
        [ "$load" = "mariadb|${conn}--sandbox -u$u scribodo|pw=$lpw|stdin=CREATE TABLE t (x INT);" ] &&
        [ "$drop" = "mariadb|${conn}-uroot|pw=$pw|stdin=DROP USER IF EXISTS '$u'@'$host';" ]
    check "$mode: load_dump loads as a one-time sandboxed user that can reach only that database, then drops it" $?

    reset
    printf 'x\n' | FAKE_SANDBOX_RC=3 lib load_dump scribodo; r=$?
    [ "$r" = 3 ] && grep -q "DROP USER IF EXISTS" "$CLIENT_LOG"
    check "$mode: a failed load returns the client's status and still drops the user" $?
done

# ---- Qdrant: one header, the app's ---------------------------------------------------------------------------------
reset
SCRIBODO_SERVICES=docker lib qdrant_delete_collections scribodo_test_x
[ "$(grep -c '^curl ' "$SHIM_LOG")" = 2 ] &&
    grep -qxF "curl http://127.0.0.1:6399/collections|Authorization: Bearer qkey-test" "$SHIM_LOG" &&
    grep -qxF "curl http://127.0.0.1:6399/collections/scribodo_test_x__p1|Authorization: Bearer qkey-test" "$SHIM_LOG"
check "qdrant_delete_collections lists, then deletes only that database's collections, with the Bearer key" $?
! grep -n "api-key" tools/services.sh tools/services_lib.sh >/dev/null &&
    [ "$(grep -c 'Authorization: Bearer' tools/services.sh tools/services_lib.sh | awk -F: '{s+=$2} END {print s}')" = 1 ]
check "the scripts send Qdrant's key one way, in one place" $?

# ---- services.sh in docker mode ------------------------------------------------------------------------------------
export SCRIBODO_SERVICES=docker
rm -f "$T/secrets/db_password" "$T/secrets/qdrant.env"
reset
out="$(tools/services.sh up 2>&1)"; r=$?
[ "$r" = 0 ] && [ "$(sed -n 1p "$SHIM_LOG")" = "docker [info]" ] &&
    [ "$(sed -n 2p "$SHIM_LOG")" = "$compose_prefix [up] [-d] [--wait] [--wait-timeout] [180]" ] &&
    [ "$(sed -n 3p "$SHIM_LOG")" = "$compose_prefix [ps] [--format] [table {{.Service}}\t{{.Status}}\t{{.Ports}}]" ] &&
    [ "$(grep -c . "$SHIM_LOG")" = 3 ]
check "up: docker info, compose up --wait, compose ps" $?
[ "$(cat "$SHIM_ENV")" = "$(. tools/services.defaults; printf '%s' "$SCRIBODO_MARIADB_SERVER_FLAGS")" ] && [ -n "$(cat "$SHIM_ENV")" ]
check "up: Compose runs with the server flags it requires exported" $?
[ "$(stat -f %Lp "$T/secrets/db_password")" = 644 ] && [ "$(stat -f %Lp "$T/secrets")" = 700 ] &&
    grep -q '^QDRANT__SERVICE__API_KEY=qkey-test$' "$T/secrets/qdrant.env" && [ ! -e "$T/secrets/mode" ]
check "up: the containers' secrets readable inside the 0700 folder; a forced mode is not remembered" $?

reset
r=0; out="$(SHIM_DOCKER_INFO=1 tools/services.sh up 2>&1)" || r=$?
[ "$r" = 6 ] && printf '%s' "$out" | tail -1 | grep -q "Docker isn't running" && [ "$(grep -c . "$SHIM_LOG")" = 1 ]
check "up without Docker running: exit 6, said, nothing else called" $?

reset
tools/services.sh down >/dev/null 2>&1; r1=$?
tools/services.sh status >/dev/null 2>&1; r2=$?
tools/services.sh logs >/dev/null 2>&1; r3=$?
[ "$r1$r2$r3" = 000 ] && [ "$(sed -n 1p "$SHIM_LOG")" = "$compose_prefix [down]" ] &&
    [ "$(sed -n 2p "$SHIM_LOG")" = "$compose_prefix [ps] [--format] [table {{.Service}}\t{{.Status}}\t{{.Ports}}]" ] &&
    [ "$(sed -n 3p "$SHIM_LOG")" = "$compose_prefix [logs] [--tail] [50]" ]
check "down, status, logs: compose down, ps, logs" $?

reset
r=0; SCRIBODO_COMPOSE_PROJECT=scribodo tools/services.sh purge >/dev/null 2>&1 || r=$?
[ "$r" = 2 ] && ! grep -q "\[down\]" "$SHIM_LOG"
check "purge refuses the author's compose project without SCRIBODO_PURGE_CONFIRM" $?
reset
tools/services.sh purge >/dev/null 2>&1; r=$?
[ "$r" = 0 ] && [ "$(cat "$SHIM_LOG")" = "$compose_prefix [down] [-v] [--remove-orphans]" ]
check "purge of a throwaway project removes its volumes" $?

reset
tools/services.sh testdb create scribodo_test_x >/dev/null 2>&1; r=$?
[ "$r" = 0 ] && [ "$(cat "$CLIENT_LOG")" = "mariadb|-uroot|pw=rootpw-docker|stdin=CREATE DATABASE IF NOT EXISTS \`scribodo_test_x\` CHARACTER SET utf8mb4; GRANT ALL ON \`scribodo_test_x\`.* TO 'scribodo'@'%';" ]
check "testdb create: the database and its grant, as root in the container" $?

# ---- Compose is called only through the library ------------------------------------------------------------------
# docker/compose.yml requires variables only services_lib.sh exports: a direct `docker compose` elsewhere fails at parse
# time, and a check reading its (empty) output passes vacuously (the red-team finding on servicestest and backuptest).
direct="$(grep -nE '(^|[^#]*[;&|({ ]|^[[:space:]]*)docker compose' tools/*.sh tests/*.sh Makefile .github/workflows/*.yml 2>/dev/null |
          grep -v '^tools/services_lib.sh:' | grep -v '^tests/test_services_docker.sh:' | grep -vE '^[^:]+:[0-9]+:[[:space:]]*#')"
[ -z "$direct" ] || printf '      %s\n' "$direct"
[ -z "$direct" ]
check "no script calls docker compose except through services_lib.sh's compose" $?

# ---- migrate-from-docker's refusals --------------------------------------------------------------------------------
reset
r=0; out="$(tools/services.sh migrate-from-docker 2>&1)" || r=$?
[ "$r" = 2 ] && printf '%s' "$out" | grep -q "unset SCRIBODO_SERVICES" && [ ! -s "$SHIM_LOG" ]
check "migrate-from-docker refuses a forced mode, touching nothing" $?
unset SCRIBODO_SERVICES
reset
r=0; out="$(SCRIBODO_QDRANT_BIN="$T/qdrant" SHIM_DOCKER_INFO=1 tools/services.sh migrate-from-docker 2>&1)" || r=$?
[ "$r" = 6 ] && printf '%s' "$out" | tail -1 | grep -q "Docker isn't running" && [ ! -e "$T/secrets/mode" ] &&
    [ ! -d "$T/secrets/mariadb" ]
check "migrate-from-docker without Docker: exit 6, nothing written" $?

exit $fail
