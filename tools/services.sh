#!/bin/bash
# tools/services.sh up|down|status|logs
#
# Starts Scribodo's local services (MariaDB, Qdrant) with Docker Compose.
# Credentials are generated once into a 0700 folder in Application Support
# and never printed. Everything binds to 127.0.0.1.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="$HOME/.docker/bin:/usr/local/bin:/opt/homebrew/bin:$PATH"
export SCRIBODO_SECRETS="${SCRIBODO_SECRETS:-$HOME/Library/Application Support/Scribodo/services}"

secrets() {
    mkdir -p "$SCRIBODO_SECRETS"
    chmod 700 "$SCRIBODO_SECRETS"
    umask 077
    gen() { openssl rand -hex 24; }
    [ -s "$SCRIBODO_SECRETS/db_password" ] || gen > "$SCRIBODO_SECRETS/db_password"
    [ -s "$SCRIBODO_SECRETS/db_root_password" ] || gen > "$SCRIBODO_SECRETS/db_root_password"
    [ -s "$SCRIBODO_SECRETS/qdrant_api_key" ] || gen > "$SCRIBODO_SECRETS/qdrant_api_key"
    printf 'QDRANT__SERVICE__API_KEY=%s\n' "$(cat "$SCRIBODO_SECRETS/qdrant_api_key")" > "$SCRIBODO_SECRETS/qdrant.env"
    # The containers read the secret files as another user: readable by the
    # Docker VM, still inside a 0700 folder on this Mac.
    chmod 644 "$SCRIBODO_SECRETS/db_password" "$SCRIBODO_SECRETS/db_root_password"
}

compose() { docker compose -f "$here/docker/compose.yml" "$@"; }

case "${1:-status}" in
    up)
        docker info >/dev/null 2>&1 || { echo "Docker isn't running (start Docker Desktop)" >&2; exit 1; }
        secrets
        compose up -d --wait --wait-timeout 180
        compose ps --format 'table {{.Service}}\t{{.Status}}\t{{.Ports}}'
        ;;
    testdb) # testdb create|drop NAME: a throwaway database for tests
        name="${3:?name}"
        case "$name" in scribodo_test_*) ;; *) echo "test databases are named scribodo_test_*" >&2; exit 2 ;; esac
        root="$(cat "$SCRIBODO_SECRETS/db_root_password")"
        if [ "${2:-}" = create ]; then
            compose exec -T mariadb mariadb -uroot -p"$root" -e "CREATE DATABASE IF NOT EXISTS \`$name\` CHARACTER SET utf8mb4; GRANT ALL ON \`$name\`.* TO 'scribodo'@'%';"
        else
            compose exec -T mariadb mariadb -uroot -p"$root" -e "DROP DATABASE IF EXISTS \`$name\`;"
            # and its semantic index collections ("<database>__<project>")
            key="$(cat "$SCRIBODO_SECRETS/qdrant_api_key")"
            for c in $(curl -s -H "Authorization: Bearer $key" http://127.0.0.1:6333/collections |
                       python3 -c 'import json,sys; print(" ".join(c["name"] for c in json.load(sys.stdin)["result"]["collections"]))'); do
                case "$c" in "${name}__"*) curl -s -o /dev/null -X DELETE -H "Authorization: Bearer $key" "http://127.0.0.1:6333/collections/$c" ;; esac
            done
        fi
        ;;
    down) compose down ;;
    status) compose ps --format 'table {{.Service}}\t{{.Status}}\t{{.Ports}}' ;;
    logs) compose logs --tail 50 ;;
    *) echo "usage: $0 up|down|status|logs|testdb create|drop NAME" >&2; exit 2 ;;
esac
