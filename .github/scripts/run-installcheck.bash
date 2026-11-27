#!/bin/bash

# Run installcheck tests for WHPG, collecting logs and diffs on failure.

set -eox pipefail

# Required configuration (from workflow env)
: "${WHPG_SRC:?WHPG_SRC not set}"
: "${RESULTS_DIR:?RESULTS_DIR not set}"
: "${TEST_TARGET:?TEST_TARGET not set}"
: "${MAKE_FLAGS:?MAKE_FLAGS not set}"
: "${PGOPTIONS:?PGOPTIONS not set}"
: "${WHPG_MAJORVERSION:?WHPG_MAJORVERSION not set}"

# Source environment explicitly (no login shell / .bash_profile dependency)
source /usr/local/greenplum-db-devel/greenplum_path.sh
[ -f ~/gpdb_src/gpAux/gpdemo/gpdemo-env.sh ] && source ~/gpdb_src/gpAux/gpdemo/gpdemo-env.sh

# Source common functions
source "${WHPG_SRC}/concourse/scripts/common.bash"

function setup_results_dir() {
    mkdir -p "${RESULTS_DIR}"
    chown gpadmin:gpadmin "${RESULTS_DIR}"
    export RESULTS_DIR
}

function look4diffs() {
    echo "========================================================================"
    echo "Test failed - collecting logs and diffs"
    echo "========================================================================"

    # Collect gpAdminLogs
    if [ -d /home/gpadmin/gpAdminLogs ]; then
        pushd /home/gpadmin/gpAdminLogs
        # Rename files with ':' in name (GitHub artifact upload issue)
        for f in *:*; do
            [ -e "$f" ] && mv -v -- "$f" "$(echo $f | tr ':' '-')"
        done
        cp -v *.* "${RESULTS_DIR}/" 2>/dev/null || true
        popd
    fi

    # Collect regression.diffs
    diff_files=$(find "${WHPG_SRC}" -name regression.diffs 2>/dev/null || true)
    for diff_file in ${diff_files}; do
        if [ -f "${diff_file}" ]; then
            # Strip WHPG_SRC prefix and convert slashes to dashes
            diff_file_copy=$(echo "${diff_file#${WHPG_SRC}/}" | tr '/' '-')
            cp "${diff_file}" "${RESULTS_DIR}/${diff_file_copy}"

            cat <<-EOF

			======================================================================
			DIFF FILE: ${diff_file}
			----------------------------------------------------------------------

			$(cat "${diff_file}")

			EOF
        fi
    done

    # Collect coordinator configs and logs
    local coord_dir="${WHPG_SRC}/gpAux/gpdemo/datadirs/qddir/demoDataDir-1"
    if [ -d "${coord_dir}" ]; then
        cp "${coord_dir}"/*.conf "${RESULTS_DIR}/" 2>/dev/null || true
        [ -d "${coord_dir}/log" ] && cp "${coord_dir}"/log/*.* "${RESULTS_DIR}/" 2>/dev/null || true
        [ -d "${coord_dir}/pg_log" ] && cp "${coord_dir}"/pg_log/*.* "${RESULTS_DIR}/" 2>/dev/null || true
    fi

    echo "Collected files in ${RESULTS_DIR}:"
    ls -la "${RESULTS_DIR}/"
}

# The "extras" target: the small suites that neither installcheck-good nor
# installcheck-world reaches, one after the other on the same cluster.  A
# failing suite does not stop the ones after it; the target fails at the end
# if any of them failed.
#
#   ic-proxy     isolation2_ic_proxy_schedule, needs the proxy mesh that
#                make_cluster set up (see the workflow)
#   ic-tcp       isolation2_ic_tcp_schedule
#   icudp        src/test/regress installcheck-icudp
#   gpmgmt-unit  gpMgmt/bin make check
#   pitr         src/test/gpdb_pitr
function run_extras() {
    local failed=""

    function run_suite() {
        local name="$1"
        shift
        echo "========================================================================"
        echo "extras: ${name}: $*"
        echo "========================================================================"
        if ( cd "${WHPG_SRC}" && "$@" ); then
            echo "extras: ${name} passed"
        else
            echo "extras: ${name} FAILED"
            failed="${failed} ${name}"
        fi
        # the next suite needs a cluster that is up
        psql -d postgres -Atc "select 1" >/dev/null 2>&1 || gpstart -a || true
    }

    function gpmgmt_unit() {
        local rc=0
        make -C gpMgmt/bin check || rc=$?
        cat gpMgmt/gpMgmt_testunit_results.log || true
        [ ${rc} -eq 0 ] || return ${rc}
        ! grep -E '^(FAILED|ERROR)' gpMgmt/gpMgmt_testunit_results.log
    }

    # pitr first: it wants the timelines of a fresh cluster.  It ends on a
    # restored copy of the cluster, its cleanup brings the demo cluster back.
    run_suite pitr make -C src/test/gpdb_pitr installcheck
    ( cd "${WHPG_SRC}/src/test/gpdb_pitr" && ./test_gpdb_pitr_cleanup.sh ) || true
    run_suite ic-proxy make -s ${MAKE_FLAGS} -C src/test/isolation2 installcheck-ic-proxy \
        PGOPTIONS="${PGOPTIONS} -c gp_interconnect_type=proxy"
    run_suite ic-tcp make -s ${MAKE_FLAGS} -C src/test/isolation2 installcheck-ic-tcp \
        PGOPTIONS="${PGOPTIONS} -c gp_interconnect_type=tcp"
    run_suite icudp make -s ${MAKE_FLAGS} -C src/test/regress installcheck-icudp \
        PGOPTIONS="${PGOPTIONS}"
    run_suite gpmgmt-unit gpmgmt_unit

    if [ -n "${failed}" ]; then
        echo "extras: FAILED suites:${failed}"
        return 1
    fi
    echo "extras: all suites passed"
}

function run_installcheck() {
    local test_target="${TEST_TARGET}"

    echo "========================================================================"
    echo "Running installcheck: ${test_target}"
    echo "WHPG_MAJORVERSION: ${WHPG_MAJORVERSION}"
    echo "========================================================================"

    # Set up error trap
    trap look4diffs ERR

    cd "${WHPG_SRC}"

    # Enable core dumps
    ulimit -c unlimited
    echo "${RESULTS_DIR}/core-%p" | sudo tee /proc/sys/kernel/core_pattern || true

    # Determine which directory to run from based on target.
    # installcheck-world and installcheck-hot-standby are top-level targets
    # (the latter runs the regress hot-standby tests, then the isolation2
    # hot_standby schedule); installcheck-small/-good live in src/test/regress.
    if [[ "${test_target}" == "installcheck-world" || "${test_target}" == "installcheck-hot-standby" ]]; then
        cd "${WHPG_SRC}"
    else
        cd "${WHPG_SRC}/src/test/regress"
    fi

    # Run tests based on version
    if [[ "${test_target}" == "extras" ]]; then
        run_extras
    elif [[ "${WHPG_MAJORVERSION}" == 6* ]]; then
        # WHPG 6: Test PL/Python3 first
        make installcheck -C "${WHPG_SRC}/src/pl/plpython" python_majorversion=3

        export TEST_PGFDW=1
        make -s ${MAKE_FLAGS} PGOPTIONS="${PGOPTIONS}" ${test_target}
    else
        # WHPG 7+
        PG_TEST_EXTRA="kerberos ssl" make -s ${MAKE_FLAGS} PGOPTIONS="${PGOPTIONS}" ${test_target}
    fi

    echo "========================================================================"
    echo "Installcheck completed successfully"
    echo "========================================================================"
}

function _main() {
    echo "TEST_TARGET: ${TEST_TARGET}"
    echo "WHPG_MAJORVERSION: ${WHPG_MAJORVERSION}"
    echo "WHPG_SRC: ${WHPG_SRC}"

    setup_results_dir
    run_installcheck
}

# Run as gpadmin if we're root
if [ "$(id -u)" = "0" ]; then
    SCRIPT_PATH="$(realpath ${BASH_SOURCE[0]})"

    # Export all required variables so they're inherited by the su subshell
    export RESULTS_DIR TEST_TARGET MAKE_FLAGS PGOPTIONS WHPG_MAJORVERSION WHPG_SRC

    su gpadmin -c "bash ${SCRIPT_PATH}"
else
    _main "$@"
fi
