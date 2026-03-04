#!/bin/bash -l

# Run installcheck tests for WHPG, collecting logs and diffs on failure.

set -eox pipefail

# Required configuration (from workflow env)
: "${GPDB_SRC:?GPDB_SRC not set}"
: "${RESULTS_DIR:?RESULTS_DIR not set}"
: "${MAKE_TEST_COMMAND:?MAKE_TEST_COMMAND not set}"
: "${GPVERSION:?GPVERSION not set}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source common functions
source "${GPDB_SRC}/concourse/scripts/common.bash"

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
    diff_files=$(find "${GPDB_SRC}" -name regression.diffs 2>/dev/null || true)
    for diff_file in ${diff_files}; do
        if [ -f "${diff_file}" ]; then
            diff_file_copy=$(echo "${diff_file#*/gpdb_src/}" | tr '/' '-')
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
    local coord_dir="${GPDB_SRC}/gpAux/gpdemo/datadirs/qddir/demoDataDir-1"
    if [ -d "${coord_dir}" ]; then
        cp "${coord_dir}"/*.conf "${RESULTS_DIR}/" 2>/dev/null || true
        [ -d "${coord_dir}/log" ] && cp "${coord_dir}"/log/*.* "${RESULTS_DIR}/" 2>/dev/null || true
        [ -d "${coord_dir}/pg_log" ] && cp "${coord_dir}"/pg_log/*.* "${RESULTS_DIR}/" 2>/dev/null || true
    fi

    echo "Collected files in ${RESULTS_DIR}:"
    ls -la "${RESULTS_DIR}/"
}

function run_installcheck() {
    local test_target="${MAKE_TEST_COMMAND}"
    local gpversion="${GPVERSION}"

    echo "========================================================================"
    echo "Running installcheck: ${test_target}"
    echo "GPVERSION: ${gpversion}"
    echo "========================================================================"

    # Set up error trap
    trap look4diffs ERR

    # Source environment
    source /usr/local/greenplum-db-devel/greenplum_path.sh
    cd "${GPDB_SRC}"
    source gpAux/gpdemo/gpdemo-env.sh

    # Enable core dumps
    ulimit -c unlimited
    echo "${RESULTS_DIR}/core-%p" | sudo tee /proc/sys/kernel/core_pattern || true

    # Determine which directory to run from based on target
    # installcheck-world runs from root, installcheck-small from src/test/regress
    if [[ "${test_target}" == "installcheck-world" ]]; then
        cd "${GPDB_SRC}"
    else
        cd "${GPDB_SRC}/src/test/regress"
    fi

    # Run tests based on version
    if [[ "${gpversion}" == 6* ]]; then
        # WHPG 6: Test PL/Python3 first
        make installcheck -C "${GPDB_SRC}/src/pl/plpython" python_majorversion=3 || true

        export TEST_PGFDW=1
        make -s ${test_target}
    else
        # WHPG 7+
        PG_TEST_EXTRA="kerberos ssl" make -s ${test_target}
    fi

    echo "========================================================================"
    echo "Installcheck completed successfully"
    echo "========================================================================"
}

function _main() {
    echo "MAKE_TEST_COMMAND: ${MAKE_TEST_COMMAND}"
    echo "GPVERSION: ${GPVERSION}"
    echo "GPDB_SRC: ${GPDB_SRC}"

    setup_results_dir
    run_installcheck
}

# Run as gpadmin if we're root
if [ "$(id -u)" = "0" ]; then
    export RESULTS_DIR MAKE_TEST_COMMAND GPVERSION GPDB_SRC
    su gpadmin -c "RESULTS_DIR='${RESULTS_DIR}' MAKE_TEST_COMMAND='${MAKE_TEST_COMMAND}' GPVERSION='${GPVERSION}' GPDB_SRC='${GPDB_SRC}' bash ${BASH_SOURCE[0]}"
else
    _main "$@"
fi
