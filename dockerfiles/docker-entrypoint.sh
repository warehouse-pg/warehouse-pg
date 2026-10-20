#!/usr/bin/env bash
# Starts sshd, then initialises (first start) or restarts (later starts) a
# WarehousePG demo cluster whose files live under /data.
#
# Environment:
#   WHPG_SEGMENTS   number of primary segments           (default 2)
#   WHPG_MIRRORS    "true" adds mirrors and a standby    (default false)
#   WHPG_PORT       coordinator port                     (default 5432)
#   WHPG_PASSWORD   gpadmin password; enables remote logins (scram-sha-256)
#
# Run with a fixed hostname (docker run --hostname whpg ...): the cluster
# records it, and a restart under a different hostname is refused.
#
# Also pass --stop-timeout 60: a clean shutdown takes about 10s, which is
# Docker's default grace period before it sends SIGKILL.
set -euo pipefail

GPHOME=${GPHOME:-/usr/local/greenplum-db-devel}
DATA_ROOT=/data
SEGMENTS=${WHPG_SEGMENTS:-2}
MIRRORS=${WHPG_MIRRORS:-false}
PORT=${WHPG_PORT:-5432}
DEMO_DIR=$DATA_ROOT/demo
QD_DIR=$DEMO_DIR/datadirs/qddir/demoDataDir-1

as_gpadmin() {
    runuser -u gpadmin -- bash -c "source $GPHOME/greenplum_path.sh; $*"
}

setup_ssh() {
    ssh-keygen -A >/dev/null
    mkdir -p /run/sshd
    /usr/sbin/sshd

    local ssh_dir=/home/gpadmin/.ssh
    if [ ! -f "$ssh_dir/id_rsa" ]; then
        runuser -u gpadmin -- bash -c "mkdir -p $ssh_dir && ssh-keygen -q -t rsa -N '' -f $ssh_dir/id_rsa \
            && cp $ssh_dir/id_rsa.pub $ssh_dir/authorized_keys \
            && printf 'Host *\n  StrictHostKeyChecking no\n  UserKnownHostsFile /dev/null\n  LogLevel ERROR\n' > $ssh_dir/config \
            && chmod 700 $ssh_dir && chmod 600 $ssh_dir/authorized_keys $ssh_dir/config"
    fi
}

init_cluster() {
    echo "Initialising WarehousePG demo cluster: $SEGMENTS segment(s), mirrors=$MIRRORS, port=$PORT"
    mkdir -p "$DEMO_DIR"
    chown gpadmin:gpadmin "$DEMO_DIR"
    cp -r /opt/whpg/gpdemo/. "$DEMO_DIR"/
    chown -R gpadmin:gpadmin "$DEMO_DIR"
    hostname > "$DATA_ROOT/.whpg-hostname"

    local standby=false
    [ "$MIRRORS" = "true" ] && standby=true
    as_gpadmin "cd $DEMO_DIR \
        && GPHOME=$GPHOME DEMO_PORT_BASE=$PORT NUM_PRIMARY_MIRROR_PAIRS=$SEGMENTS \
           WITH_MIRRORS=$MIRRORS WITH_STANDBY=$standby DEFAULT_QD_MAX_CONNECT=150 \
           DATADIRS=$DEMO_DIR/datadirs ./demo_cluster.sh"

    if [ -n "${WHPG_PASSWORD:-}" ]; then
        # Remote logins need a password; local socket access stays as configured.
        as_gpadmin "export COORDINATOR_DATA_DIRECTORY=$QD_DIR PGPORT=$PORT; \
            psql -d postgres -v ON_ERROR_STOP=1 \
                -c \"SET password_encryption = 'scram-sha-256'\" \
                -c \"ALTER ROLE gpadmin PASSWORD '${WHPG_PASSWORD//\'/\'\'}'\" \
            && sed -i '1i host all all 0.0.0.0/0 scram-sha-256' $QD_DIR/pg_hba.conf \
            && gpstop -u -a"
    fi
}

start_cluster() {
    local saved
    saved=$(cat "$DATA_ROOT/.whpg-hostname" 2>/dev/null || true)
    if [ -n "$saved" ] && [ "$saved" != "$(hostname)" ]; then
        echo "ERROR: cluster was created under hostname '$saved' but this container is '$(hostname)'." >&2
        echo "       Re-run with: docker run --hostname $saved ..." >&2
        exit 1
    fi
    echo "Starting existing WarehousePG cluster"
    as_gpadmin "export COORDINATOR_DATA_DIRECTORY=$QD_DIR; gpstart -a"
}

stop_cluster() {
    echo "Stopping WarehousePG cluster"
    as_gpadmin "export COORDINATOR_DATA_DIRECTORY=$QD_DIR; gpstop -a -M fast" || true
    exit 0
}

# Pass a command through (e.g. `docker run ... bash`) without touching the cluster.
if [ "$#" -gt 0 ]; then
    exec "$@"
fi

chown gpadmin:gpadmin "$DATA_ROOT"
setup_ssh
trap stop_cluster TERM INT

if [ -f "$QD_DIR/PG_VERSION" ]; then
    start_cluster
else
    init_cluster
fi

echo "WarehousePG is ready on port $PORT (database: postgres, user: gpadmin)"
tail -f /dev/null &
wait $!
