#!/usr/bin/env bash
# Verifies a running container: waits for the cluster, then checks the version,
# segment health and a small distributed query.
# Usage: whpg-smoke-test            (inside the container)
#        docker exec <container> whpg-smoke-test
set -euo pipefail

GPHOME=${GPHOME:-/usr/local/greenplum-db-devel}
PORT=${WHPG_PORT:-5432}

# Local socket access uses peer authentication, so run as the cluster owner.
if [ "$(id -u)" -eq 0 ]; then
    exec runuser -u gpadmin -- env WHPG_PORT="$PORT" GPHOME="$GPHOME" "$0" "$@"
fi

source "$GPHOME/greenplum_path.sh"
export PGPORT=$PORT

ready=false
for _ in $(seq 1 120); do
    if psql -d postgres -Atqc 'select 1' >/dev/null 2>&1; then
        ready=true
        break
    fi
    sleep 2
done
if [ "$ready" != true ]; then
    echo "smoke test failed: cluster did not accept connections within 240s" >&2
    exit 1
fi

psql -d postgres -v ON_ERROR_STOP=1 -Atq <<'SQL'
select version();
select 'segments up: ' || count(*) from gp_segment_configuration where status = 'u';
create temp table smoke as select g as id from generate_series(1, 1000) g distributed by (id);
select 'distributed rows: ' || count(*) from smoke;
SQL
echo "smoke test passed"
