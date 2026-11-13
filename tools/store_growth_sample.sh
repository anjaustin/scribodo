#!/bin/bash
# One line of store size and backlog for a disposable test database (GAP-003 soak): the database's data and
# index bytes, row counts, the memory outbox's pending deliveries, the observer's lag (messages past the point
# each conversation was reviewed to) and the points held by the database's semantic collections. Read-only.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
usage() {
    echo 'usage: tools/store_growth_sample.sh scribodo_test_NAME [LABEL]'
    echo 'Prints: STORE_GROWTH label=L at=UTC db_bytes=N messages=N memory_units=N records=N revisions=N'
    echo '        outbox_pending=N outbox_leased=N outbox_failed=N observer_lag=N scope_cursor=N scope_pending=N max_seq=N qdrant_points=N'
    echo '        (scope_cursor: the lowest ingest cursor of the enabled memory scopes; scope_pending: the highest pending sequence; -1 none)'
    echo 'Requires tools/services.sh up; reads the database through tools/services.sh testdb sql (read-only).'
}
[[ $# -ge 1 && $# -le 2 ]] || { usage >&2; exit 2; }
name=$1; label=${2:-sample}
[[ $name =~ ^scribodo_test_[a-z0-9_]+$ ]] || { usage >&2; exit 2; }
[[ $label =~ ^[A-Za-z0-9_.:-]+$ ]] || { echo 'label: letters, digits, _ . : -' >&2; exit 2; }
cd "$here"
# Every query names the database explicitly. A table the application has not created yet (the database is
# empty until the app opens it) counts as 0: the counts are built over the tables that exist.
tables=$("$here/tools/services.sh" testdb sql "$name" <<SQL | tail -n +2 | tr '\n' ' '
SELECT table_name FROM information_schema.tables WHERE table_schema = '$name';
SQL
)
has() { [[ " $tables " == *" $1 "* ]]; }
count() { if has "$1"; then echo "(SELECT COUNT(*) FROM \`$name\`.\`$1\`${2:+ WHERE $2})"; else echo 0; fi; }
if has messages; then
    lag="(SELECT COALESCE(SUM(GREATEST(m.n - COALESCE(o.msg, 0), 0)), 0)
          FROM (SELECT conversation_id, COUNT(*) AS n FROM \`$name\`.messages GROUP BY conversation_id) m
          LEFT JOIN $(has observer_chats && echo "\`$name\`.observer_chats" || echo "(SELECT NULL AS conversation_id, 0 AS msg) ") o ON o.conversation_id = m.conversation_id)"
else
    lag=0
fi
read -r db_bytes messages units records revisions outbox leased failed lag cursor pending maxseq < <(
    "$here/tools/services.sh" testdb sql "$name" <<SQL | tail -1
SELECT
  COALESCE((SELECT SUM(data_length + index_length) FROM information_schema.tables WHERE table_schema = '$name'), 0),
  $(count messages), $(count memory_units), $(count records), $(count revisions),
  $(count memory_outbox "state = 'pending'"), $(count memory_outbox "state = 'leased'"), $(count memory_outbox "state = 'failed'"),
  $lag,
  $(has memory_scopes && echo "COALESCE((SELECT MIN(cursor_seq) FROM \`$name\`.memory_scopes WHERE enabled = 1), -1)" || echo -1),
  $(has memory_scopes && echo "COALESCE((SELECT MAX(pending_seq) FROM \`$name\`.memory_scopes WHERE enabled = 1), -1)" || echo -1),
  $(has messages && echo "COALESCE((SELECT MAX(seq) FROM \`$name\`.messages), -1)" || echo -1);
SQL
)
key="$(cat "${SCRIBODO_SECRETS:-$HOME/Library/Application Support/Scribodo/services}/qdrant_api_key")"
qport="${SCRIBODO_QDRANT_PORT:-6333}"
points=$(curl -s -H @<(printf 'Authorization: Bearer %s\n' "$key") "http://127.0.0.1:$qport/collections" |
    python3 -c 'import json,sys; print(" ".join(c["name"] for c in json.load(sys.stdin)["result"]["collections"]))' |
    tr ' ' '\n' | { grep "^${name}__" || true; } | while read -r c; do  # no collection yet is 0, not a failed pipeline
        curl -s -H @<(printf 'Authorization: Bearer %s\n' "$key") "http://127.0.0.1:$qport/collections/$c" |
            python3 -c 'import json,sys; print(json.load(sys.stdin)["result"].get("points_count") or 0)'
    done | awk '{s+=$1} END{print s+0}')
echo "STORE_GROWTH label=$label at=$(date -u +%Y-%m-%dT%H:%M:%SZ) db_bytes=$db_bytes messages=$messages memory_units=$units records=$records revisions=$revisions outbox_pending=$outbox outbox_leased=$leased outbox_failed=$failed observer_lag=$lag scope_cursor=$cursor scope_pending=$pending max_seq=$maxseq qdrant_points=${points:-0}"
