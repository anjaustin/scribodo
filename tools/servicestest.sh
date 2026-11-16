#!/bin/bash
# make servicestest: the native services mode (tools/services.sh) rehearsed end to end on throwaway stores, never the
# author's: each step runs with its own secrets folder, logs and ports (a "default" store lives under a throwaway HOME).
# What REDTEAM-023 found is pinned here:
#   1. a fresh store comes up through a pipe without hanging it; the install's extra accounts are gone; root needs its
#      password over TCP; Qdrant needs its key; neither service answers on the network address
#   2. two `up`s at once start one MariaDB and one Qdrant
#   3. a first start interrupted before its setup finished is finished by the next `up`
#   4. a stale pid file naming another program neither stops Qdrant starting nor makes `down` signal that program
#   5. the daemons hold none of the caller's descriptors and lead their own process groups
#   6. purge refuses the default store however its path is spelled, before stopping anything; a purged native store
#      comes back native
#   7. credentials without a native store (someone who used the containers) never start an empty native store
#   8. the socket of a store with a long path lives in a private directory
#   9. migrate-from-docker refuses while the app runs, on the author's store
#  10. with Docker running (SCRIBODO_SERVICESTEST_DOCKER=1 requires it): migrate-from-docker moves a store and keeps the
#      dump; a second migration over the native store is refused and changes nothing; a migration whose load fails puts
#      both stores back as they were, and a replacing one leaves the native store exactly as it was; a migration over a
#      volume made under other credentials is refused by name and leaves the running native store running
# Logs of every store are kept in build/servicestest-logs (uploaded by CI on failure).
set -u
cd "$(dirname "$0")/.."
MB="${SCRIBODO_MARIADB_BIN:-/opt/homebrew/opt/mariadb@11.4/bin}"
root="$(cd "$(mktemp -d /tmp/scribodo-servicestest.XXXXXX)" && pwd -P)"   # one spelling, as services_lib.sh uses (/private/tmp)
keep_logs="$PWD/build/servicestest-logs"
fail=0
step() { printf '%-78s' "$1"; }
ok() { if [ "$1" = 0 ]; then echo ok; else echo FAIL; fail=1; fi; }
store() { # store NAME DBPORT QPORT: the environment of one throwaway store
    export SCRIBODO_SECRETS="$root/$1" SCRIBODO_LOGS="$root/logs-$1" SCRIBODO_DB_PORT="$2" SCRIBODO_QDRANT_PORT="$3"
    export SCRIBODO_SERVICES=native SCRIBODO_COMPOSE_PROJECT="scribodo-st-$1"
}
svc() { tools/services.sh "$@"; }
# Compose through the library, which exports what docker/compose.yml requires (the server flags, the ports): a bare
# `docker compose` fails at parse time and its empty output would pass a "no containers" check (RED-TEAM, AUDIT-004).
lib_compose() { bash -c '. tools/services_lib.sh; compose "$@"' lib "$@"; }
containers_down() { # containers_down PROJECT: true only when Compose answered and the containers are removed (-a: a
    local out                                         # stopped container that still exists is not "down")
    out="$(SCRIBODO_COMPOSE_PROJECT="$1" lib_compose ps -aq 2>/dev/null)" || return 1
    [ -z "$out" ]
}
# fresh_project X: nothing of the rehearsal's Compose project scribodo-st-X survives from an earlier, interrupted run:
# its containers, its network and its two volumes, by exact name (a stale volume keeps the root password of a deleted
# secrets folder, and every later run inherits it: 2026-11-16). Nothing else is touched: the author's pre-move store
# (scribodo_mariadb, scribodo_qdrant) must survive.
fresh_project() {
    [[ "$1" =~ ^[a-z]$ ]] || { echo "fresh_project: not a rehearsal store: $1" >&2; return 1; }
    local p="scribodo-st-$1" c v
    for c in $(docker ps -aq --filter "label=com.docker.compose.project=$p"); do docker rm -f "$c" >/dev/null || return 1; done
    for v in "${p}_mariadb" "${p}_qdrant"; do
        if docker volume inspect "$v" >/dev/null 2>&1; then docker volume rm "$v" >/dev/null || return 1; fi
    done
    if docker network inspect "${p}_default" >/dev/null 2>&1; then docker network rm "${p}_default" >/dev/null || return 1; fi
}
mdb() { MYSQL_PWD="$1" "$MB/mariadb" --no-defaults --skip-ssl-verify-server-cert -N "${@:2}"; }
cleanup() {
    while IFS= read -r d; do
        d="${d%/mariadb/data}"
        SCRIBODO_SECRETS="$d" SCRIBODO_SERVICES=native tools/services.sh down >/dev/null 2>&1
    done < <(find "$root" -type d -path '*/mariadb/data' 2>/dev/null)
    [ -n "${sleeper:-}" ] && kill "$sleeper" 2>/dev/null
    if docker info >/dev/null 2>&1; then   # step 10's containers and volumes, whether or not its secrets folder exists
        for p in e g h i; do fresh_project "$p" || echo "cleanup: scribodo-st-$p could not be removed" >&2; done
    fi
    rm -rf "$keep_logs"; mkdir -p "$keep_logs"
    find "$root" -maxdepth 1 -type d -name 'logs-*' -exec cp -R {} "$keep_logs/" \; 2>/dev/null
    rm -rf "$root"
}
trap cleanup EXIT

store a 33071 6341
step "1. a fresh store comes up through a pipe"
out="$(svc up 2>&1 | cat)"; r=${PIPESTATUS[0]}
[ "$r" = 0 ] && echo "$out" | grep -q "mariadb	running" && echo "$out" | grep -q "qdrant	running"; ok $?
step "   the setup marker exists and the mode was not remembered (it was forced)"
[ -f "$SCRIBODO_SECRETS/mariadb/initialized" ] && [ ! -e "$SCRIBODO_SECRETS/mode" ]; ok $?
rp="$(cat "$SCRIBODO_SECRETS/db_root_password")"
step "   no anonymous accounts, no root at the host name, no test database or grant"
n="$(mdb "$rp" -uroot -h127.0.0.1 -P33071 -e "SELECT (SELECT COUNT(*) FROM mysql.global_priv WHERE User='' OR (User='root' AND Host NOT IN ('localhost','127.0.0.1','::1'))) + (SELECT COUNT(*) FROM mysql.db WHERE Db LIKE 'test%') + (SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='test')")"
[ "$n" = 0 ]; ok $?
step "   root over TCP needs the password; the app's user reaches its database"
! mdb "" -uroot -h127.0.0.1 -P33071 -e "SELECT 1" >/dev/null 2>&1 &&
    mdb "$(cat "$SCRIBODO_SECRETS/db_password")" -uscribodo -h127.0.0.1 -P33071 scribodo -e "SELECT 1" >/dev/null; ok $?
step "   Qdrant refuses a request without the key; neither answers on the network address"
lan="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)"
[ "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:6341/collections)" = 401 ] &&
    { [ -z "$lan" ] || { ! nc -z -G 2 "$lan" 33071 2>/dev/null && ! nc -z -G 2 "$lan" 6341 2>/dev/null; }; }; ok $?

step "5. the daemons hold none of the caller's descriptors and lead their own groups"
svc down >/dev/null 2>&1
exec 7>"$root/held-by-caller"
svc up >/dev/null 2>&1; r=$?
mp="$(cat "$SCRIBODO_SECRETS/mariadb/mariadbd.pid" 2>/dev/null)"; qp="$(cat "$SCRIBODO_SECRETS/qdrant/qdrant.pid" 2>/dev/null)"
held="$(lsof -p "$mp,$qp" 2>/dev/null | grep -c "held-by-caller")"
# (macOS ps reports every session as 0: a session leader is its own process-group leader, outside the caller's group)
[ "$r" = 0 ] && [ -n "$mp" ] && [ -n "$qp" ] && kill -0 "$mp" && kill -0 "$qp" && [ "$held" = 0 ] && [ "$(ps -o pgid= -p "$mp" | tr -d ' ')" = "$mp" ] && [ "$(ps -o pgid= -p "$qp" | tr -d ' ')" = "$qp" ] &&
    [ "$(ps -o pgid= -p "$mp" | tr -d ' ')" != "$(ps -o pgid= -p $$ | tr -d ' ')" ]; ok $?
exec 7>&-

step "2. two \`up\`s at once start one MariaDB and one Qdrant"
svc down >/dev/null 2>&1
svc up >/dev/null 2>&1 & a=$!; svc up >/dev/null 2>&1 & b=$!; wait $a; ra=$?; wait $b; rb=$?
nm="$(pgrep -f -- "--datadir=$SCRIBODO_SECRETS/mariadb/data" | wc -l | tr -d ' ')"
nq="$(pgrep -f -- "--config-path $SCRIBODO_SECRETS/qdrant/config.yaml" | wc -l | tr -d ' ')"
[ "$ra" = 0 ] && [ "$rb" = 0 ] && [ "$nm" = 1 ] && [ "$nq" = 1 ]; ok $?

step "3. a first start interrupted before its setup is finished by the next \`up\`"
store b 33072 6342
mkdir -p "$SCRIBODO_SECRETS/mariadb" && chmod 700 "$SCRIBODO_SECRETS" "$SCRIBODO_SECRETS/mariadb"
"$MB/mariadb-install-db" --no-defaults --datadir="$SCRIBODO_SECRETS/mariadb/data" \
    --auth-root-authentication-method=normal >/dev/null 2>&1   # the data directory exists, no setup ran
SCRIBODO_NATIVE_FRESH=1 svc up >/dev/null 2>&1; r=$?
[ "$r" = 0 ] && [ -f "$SCRIBODO_SECRETS/mariadb/initialized" ] &&
    ! mdb "" -uroot -h127.0.0.1 -P33072 -e "SELECT 1" >/dev/null 2>&1 &&
    mdb "$(cat "$SCRIBODO_SECRETS/db_root_password")" -uroot -h127.0.0.1 -P33072 -e "SELECT 1" >/dev/null; ok $?
step "   and a marker removed after a completed setup is restored with the password"
rm -f "$SCRIBODO_SECRETS/mariadb/initialized"; svc up >/dev/null 2>&1; r=$?
[ "$r" = 0 ] && [ -f "$SCRIBODO_SECRETS/mariadb/initialized" ]; ok $?
svc down >/dev/null 2>&1

step "4. a stale pid file naming another program does not stop Qdrant starting"
store a 33071 6341
sleep 600 & sleeper=$!; disown
svc down >/dev/null 2>&1
echo "$sleeper" > "$SCRIBODO_SECRETS/qdrant/qdrant.pid"
svc up >/dev/null 2>&1; r=$?
[ "$r" = 0 ] && [ "$(cat "$SCRIBODO_SECRETS/qdrant/qdrant.pid")" != "$sleeper" ] && kill -0 "$sleeper" &&
    pgrep -f -- "--config-path $SCRIBODO_SECRETS/qdrant/config.yaml" >/dev/null; ok $?
step "   and \`down\` given a pid file naming another program leaves that program alone"
svc down >/dev/null 2>&1
echo "$sleeper" > "$SCRIBODO_SECRETS/qdrant/qdrant.pid"
svc down >/dev/null 2>&1; r=$?
[ "$r" = 0 ] && kill -0 "$sleeper"; ok $?

step "   a pid file naming another store's live Qdrant: \`up\` starts this one's, \`down\` spares the other"
svc down >/dev/null 2>&1
( store b 33072 6342; tools/services.sh up >/dev/null 2>&1 ) ; bq="$(cat "$root/b/qdrant/qdrant.pid" 2>/dev/null)"
echo "$bq" > "$SCRIBODO_SECRETS/qdrant/qdrant.pid"
svc up >/dev/null 2>&1; r1=$?; own="$(cat "$SCRIBODO_SECRETS/qdrant/qdrant.pid")"
svc down >/dev/null 2>&1; r2=$?
[ -n "$bq" ] && [ "$r1" = 0 ] && [ "$own" != "$bq" ] && [ "$r2" = 0 ] && kill -0 "$bq" &&
    ! pgrep -f -- "--config-path $SCRIBODO_SECRETS/qdrant/config.yaml" >/dev/null; ok $?
( store b 33072 6342; tools/services.sh down >/dev/null 2>&1 )

step "6. purge refuses the default store, however spelled, before stopping it"
fakehome="$root/home6"; mkdir -p "$fakehome/Library/Application Support/Scribodo"
( export HOME="$fakehome"; d="$fakehome/Library/Application Support/Scribodo/services"
  export SCRIBODO_SECRETS="$d" SCRIBODO_LOGS="$root/logs-home6" SCRIBODO_DB_PORT=33073 SCRIBODO_QDRANT_PORT=6343
  unset SCRIBODO_SERVICES
  tools/services.sh up >/dev/null 2>&1 || exit 1
  r1=0; SCRIBODO_SECRETS="$d/" tools/services.sh purge >/dev/null 2>&1 || r1=$?
  r2=0; SCRIBODO_SECRETS="$fakehome/Library/Application Support/Scribodo//services" tools/services.sh purge >/dev/null 2>&1 || r2=$?
  up="$(tools/services.sh status | grep -c running)"
  r3=0; SCRIBODO_PURGE_CONFIRM=1 tools/services.sh purge >/dev/null 2>&1 || r3=$?
  [ "$r1" = 2 ] && [ "$r2" = 2 ] && [ "$up" = 2 ] && [ "$r3" = 0 ] && [ ! -d "$d/mariadb" ] ); ok $?
step "   a purged native store comes back native and fresh on the next \`up\`"
( export HOME="$fakehome"; d="$fakehome/Library/Application Support/Scribodo/services"
  export SCRIBODO_SECRETS="$d" SCRIBODO_LOGS="$root/logs-home6" SCRIBODO_DB_PORT=33073 SCRIBODO_QDRANT_PORT=6343
  unset SCRIBODO_SERVICES
  m="$(tools/services.sh mode)"; tools/services.sh up >/dev/null 2>&1; r=$?
  tools/services.sh down >/dev/null 2>&1
  [ "$m" = native ] && [ "$r" = 0 ] && [ -f "$d/mariadb/initialized" ] ); ok $?

step "7. credentials without a native store never start an empty native store"
store d 33074 6344
mkdir -p "$SCRIBODO_SECRETS" && chmod 700 "$SCRIBODO_SECRETS"
openssl rand -hex 24 > "$SCRIBODO_SECRETS/db_root_password"
unset SCRIBODO_SERVICES
m="$(svc mode)"
r=0; SCRIBODO_SERVICES=native svc up >/dev/null 2>&1 || r=$?
[ "$m" = docker ] && [ "$r" = 2 ] && [ ! -d "$SCRIBODO_SECRETS/mariadb/data" ]; ok $?

step "8. the socket of a store with a long path lives in a private directory"
long="$root/a-secrets-folder-with-a-long-name-like-a-ci-runners-temporary-directory-has-$(printf 'x%.0s' $(seq 1 30))"
sockpath="$(SCRIBODO_SECRETS="$long" bash -c '. tools/services_lib.sh; native_sock')"; sockdir="$(dirname "$sockpath")"
[ "${#long}" -gt 100 ] && [ "${sockdir#/tmp/scribodo-}" != "$sockdir" ] && [ "$(stat -f %Lp "$sockdir")" = 700 ] &&
    [ -O "$sockdir" ] && [ ! -L "$sockdir" ]; ok $?
rmdir "$sockdir" 2>/dev/null

step "9. migrate-from-docker refuses while the app runs, on the author's store"
fakehome9="$root/home9"; mkdir -p "$fakehome9/Library/Application Support/Scribodo"
printf '#include <unistd.h>\nint main(void) { sleep(30); return 0; }\n' | cc -x c - -o "$root/scribodo"  # a copied Apple binary is killed
"$root/scribodo" & fake=$!
sleep 0.3
started=0; kill -0 "$fake" 2>/dev/null && started=1   # the refusal must come from this stand-in, not chance
r=0; out="$(HOME="$fakehome9" SCRIBODO_SECRETS="$fakehome9/Library/Application Support/Scribodo/services" SCRIBODO_SERVICES= \
    tools/services.sh migrate-from-docker 2>&1)" || r=$?
kill "$fake" 2>/dev/null; wait "$fake" 2>/dev/null
[ "$started" = 1 ] && [ "$r" = 2 ] && echo "$out" | grep -q "quit Scribodo first" &&
    [ ! -e "$fakehome9/Library/Application Support/Scribodo/services/mode" ]; ok $?

if docker info >/dev/null 2>&1; then
    store e 33075 6345
    unset SCRIBODO_SERVICES
    step "10. migrate-from-docker moves a store and keeps the dump"
    setup=0; fresh_project e || setup=1
    SCRIBODO_SERVICES=docker svc up >/dev/null 2>&1 || SCRIBODO_SERVICES=docker svc up >/dev/null 2>&1 || setup=1  # Docker's first start after launch can fail its health check
    SCRIBODO_SERVICES=docker svc rootsql scribodo <<'SQL' >/dev/null 2>&1 || setup=1
CREATE TABLE schema_migrations (version INT PRIMARY KEY); INSERT INTO schema_migrations VALUES (18);
CREATE TABLE probe (t TEXT); INSERT INTO probe VALUES ('the author''s words');
SQL
    [ "$setup" = 0 ] || echo "      (setup failed: the case below is not what it claims)"
    out="$(svc migrate-from-docker 2>&1)"; r=$?
    got="$(SCRIBODO_SERVICES=native svc rootsql -N scribodo -e "SELECT t FROM probe" 2>/dev/null)"
    [ "$setup" = 0 ] && [ "$r" = 0 ] && [ "$got" = "the author's words" ] && [ "$(cat "$SCRIBODO_SECRETS/mode")" = native ] &&
        ls "$SCRIBODO_SECRETS"/migration-*.sql.gz >/dev/null 2>&1 &&
        containers_down scribodo-st-e; ok $?
    [ "$r" = 0 ] && [ "$got" = "the author's words" ] || { echo "      exit $r, read back [$got]"; echo "$out" | tail -5 | sed 's/^/      /'; }
    step "    a second migration over the native store is refused and changes nothing"
    before="$(SCRIBODO_SERVICES=native svc status)"
    r=0; svc migrate-from-docker >/dev/null 2>&1 || r=$?
    got="$(SCRIBODO_SERVICES=native svc rootsql -N scribodo -e "SELECT COUNT(*) FROM probe" 2>/dev/null)"
    [ "$r" = 2 ] && [ "$got" = 1 ] && [ "$(cat "$SCRIBODO_SECRETS/mode")" = native ] && [ "$(SCRIBODO_SERVICES=native svc status)" = "$before" ] &&
        containers_down scribodo-st-e; ok $?
    SCRIBODO_SERVICES=native svc down >/dev/null 2>&1
    SCRIBODO_SERVICES=docker SCRIBODO_PURGE_CONFIRM=1 svc purge >/dev/null 2>&1

    store g 33077 6347
    unset SCRIBODO_SERVICES
    step "    a migration whose load fails puts both stores back as they were"
    setup=0; fresh_project g || setup=1
    SCRIBODO_SERVICES=docker svc up >/dev/null 2>&1 || SCRIBODO_SERVICES=docker svc up >/dev/null 2>&1 || setup=1
    # A view whose definer the restore user may not impersonate: the load fails part-way, as any dump the sandboxed
    # restore user cannot load would.
    SCRIBODO_SERVICES=docker svc rootsql scribodo <<'SQL' >/dev/null 2>&1 || setup=1
CREATE TABLE schema_migrations (version INT PRIMARY KEY); INSERT INTO schema_migrations VALUES (18);
CREATE TABLE probe (t TEXT); INSERT INTO probe VALUES ('kept');
CREATE DEFINER = 'root'@'%' SQL SECURITY DEFINER VIEW probe_view AS SELECT t FROM probe;
SQL
    [ "$setup" = 0 ] || echo "      (setup failed: the case below is not what it claims)"
    r=0; out="$(svc migrate-from-docker 2>&1)" || r=$?
    got="$(SCRIBODO_SERVICES=docker svc rootsql -N scribodo -e "SELECT t FROM probe" 2>/dev/null)"
    # It failed at the load, not earlier: the containers' dump was taken and the sandboxed user was refused the view.
    [ "$setup" = 0 ] && [ "$r" = 1 ] && echo "$out" | grep -q "dumped the containers'" && echo "$out" | grep -q "Access denied" &&
        echo "$out" | grep -q "both stores are as they were" && [ "$(cat "$SCRIBODO_SECRETS/mode")" = docker ] &&
        [ "$got" = kept ] && [ ! -d "$SCRIBODO_SECRETS/mariadb" ] && [ ! -d "$SCRIBODO_SECRETS/qdrant" ] &&
        ! pgrep -f -- "--datadir=$SCRIBODO_SECRETS/mariadb/data" >/dev/null &&
        ! pgrep -f -- "--config-path $SCRIBODO_SECRETS/qdrant/config.yaml" >/dev/null; ok $?
    [ "$r" = 1 ] || echo "$out" | tail -5 | sed 's/^/      /'
    SCRIBODO_SERVICES=docker SCRIBODO_PURGE_CONFIRM=1 svc purge >/dev/null 2>&1

    store h 33078 6348
    unset SCRIBODO_SERVICES
    step "    a replacing migration whose load fails leaves the native store exactly as it was"
    setup=0; fresh_project h || setup=1
    SCRIBODO_SERVICES=docker svc up >/dev/null 2>&1 || SCRIBODO_SERVICES=docker svc up >/dev/null 2>&1 || setup=1
    SCRIBODO_SERVICES=docker svc rootsql scribodo <<'SQL' >/dev/null 2>&1 || setup=1
CREATE TABLE schema_migrations (version INT PRIMARY KEY); INSERT INTO schema_migrations VALUES (18);
CREATE TABLE docker_only (t TEXT); INSERT INTO docker_only VALUES ('from the containers');
CREATE DEFINER = 'root'@'%' SQL SECURITY DEFINER VIEW probe_view AS SELECT t FROM docker_only;
SQL
    SCRIBODO_SERVICES=docker svc down >/dev/null 2>&1 || setup=1   # the containers were down before the migration
    SCRIBODO_SERVICES=native SCRIBODO_NATIVE_FRESH=1 svc up >/dev/null 2>&1 || setup=1
    SCRIBODO_SERVICES=native svc rootsql scribodo <<'SQL' >/dev/null 2>&1 || setup=1
CREATE TABLE probe (t TEXT); INSERT INTO probe VALUES ('native original');
SQL
    [ "$setup" = 0 ] || echo "      (setup failed: the case below is not what it claims)"
    r=0; out="$(SCRIBODO_MIGRATE_REPLACE=1 svc migrate-from-docker 2>&1)" || r=$?
    got="$(SCRIBODO_SERVICES=native svc rootsql -N scribodo -e "SELECT t FROM probe" 2>/dev/null)"
    other="$(SCRIBODO_SERVICES=native svc rootsql -N -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='scribodo' AND table_name IN ('docker_only','schema_migrations')" 2>/dev/null)"
    [ "$setup" = 0 ] && [ "$r" = 1 ] && echo "$out" | grep -q "kept the native store's previous contents" && echo "$out" | grep -q "both stores are as they were" &&
        [ "$got" = "native original" ] && [ "$other" = 0 ] && [ "$(cat "$SCRIBODO_SECRETS/mode")" = native ] &&
        [ "$(SCRIBODO_SERVICES=native svc status | grep -c running)" = 2 ] &&
        containers_down scribodo-st-h; ok $?
    [ "$r" = 1 ] && [ "$got" = "native original" ] || echo "$out" | tail -6 | sed 's/^/      /'
    SCRIBODO_SERVICES=native svc down >/dev/null 2>&1
    SCRIBODO_SERVICES=docker SCRIBODO_PURGE_CONFIRM=1 svc purge >/dev/null 2>&1

    store i 33079 6349
    unset SCRIBODO_SERVICES
    step "    a migration over a volume made under other credentials puts the running native store back"
    # The containers' volume is initialised under one root password, then the secrets folder gets another (credentials
    # rotated after the volume was made): the containers come up, but root cannot dump them. The native store is
    # running and holds data, so the restore must stop the containers before the native services come back on the
    # same ports (verification of AUDIT-004, 2026-11-16).
    setup=0; fresh_project i || setup=1
    SCRIBODO_SERVICES=docker svc up >/dev/null 2>&1 || SCRIBODO_SERVICES=docker svc up >/dev/null 2>&1 || setup=1
    SCRIBODO_SERVICES=docker svc down >/dev/null 2>&1 || setup=1
    openssl rand -hex 24 > "$SCRIBODO_SECRETS/db_root_password.new" && mv "$SCRIBODO_SECRETS/db_root_password.new" "$SCRIBODO_SECRETS/db_root_password" || setup=1
    SCRIBODO_SERVICES=native SCRIBODO_NATIVE_FRESH=1 svc up >/dev/null 2>&1 || setup=1
    printf "CREATE TABLE probe (t TEXT); INSERT INTO probe VALUES ('native original');\n" |
        SCRIBODO_SERVICES=native svc rootsql scribodo >/dev/null 2>&1 || setup=1
    [ "$setup" = 0 ] || echo "      (setup failed: the case below is not what it claims)"
    mode_before="$(cat "$SCRIBODO_SECRETS/mode" 2>/dev/null || echo none)"
    r=0; out="$(SCRIBODO_MIGRATE_REPLACE=1 svc migrate-from-docker 2>&1)" || r=$?
    got="$(SCRIBODO_SERVICES=native svc rootsql -N scribodo -e "SELECT t FROM probe" 2>/dev/null)"
    running="$(SCRIBODO_SERVICES=native svc status 2>/dev/null | grep -c running)"
    mode_after="$(cat "$SCRIBODO_SECRETS/mode" 2>/dev/null || echo none)"
    [ "$setup" = 0 ] && [ "$r" = 1 ] && echo "$out" | grep -q "root password does not match the secrets folder" &&
        echo "$out" | grep -q "both stores are as they were" && [ "$running" = 2 ] && [ "$got" = "native original" ] &&
        containers_down scribodo-st-i && [ "$mode_after" != docker ] &&
        { [ "$mode_before" = none ] || [ "$mode_after" = "$mode_before" ]; }; ok $?
    [ "$r" = 1 ] && [ "$running" = 2 ] && [ "$got" = "native original" ] ||
        { echo "      exit $r, running $running, read back [$got], mode $mode_before -> $mode_after"; echo "$out" | tail -6 | sed 's/^/      /'; }
    SCRIBODO_SERVICES=native svc down >/dev/null 2>&1
    SCRIBODO_SERVICES=docker SCRIBODO_PURGE_CONFIRM=1 svc purge >/dev/null 2>&1
else
    if [ "${SCRIBODO_SERVICESTEST_DOCKER:-}" = 1 ]; then step "10. migrate-from-docker (Docker is required and not running)"; ok 1
    else echo "10. migrate-from-docker: skipped, Docker is not running (SCRIBODO_SERVICESTEST_DOCKER=1 requires it)"; fi
fi
exit $fail
