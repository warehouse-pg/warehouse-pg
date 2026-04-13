#!/bin/bash

# Run installcheck tests for WHPG, collecting logs and diffs on failure.

set -eox pipefail

# Required configuration (from workflow env)
: "${WHPG_SRC:?WHPG_SRC not set}"
: "${RESULTS_DIR:?RESULTS_DIR not set}"
: "${MAKE_TEST_COMMAND:?MAKE_TEST_COMMAND not set}"
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

function run_installcheck() {
    local test_target="${MAKE_TEST_COMMAND}"

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

    # Determine which directory to run from based on target
    # installcheck-world runs from root, installcheck-small from src/test/regress
    if [[ "${test_target}" == "installcheck-world" ]]; then
        cd "${WHPG_SRC}"
    else
        cd "${WHPG_SRC}/src/test/regress"
    fi

    # Run tests based on version
    if [[ "${WHPG_MAJORVERSION}" == 6* ]]; then
        # WHPG 6: Test PL/Python3 first
        make installcheck -C "${WHPG_SRC}/src/pl/plpython" python_majorversion=3 || true

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
    echo "WHPG_MAJORVERSION: ${WHPG_MAJORVERSION}"
    echo "WHPG_SRC: ${WHPG_SRC}"

    setup_results_dir
    run_installcheck
}

# Run as gpadmin if we're root
if [ "$(id -u)" = "0" ]; then
    SCRIPT_PATH="$(realpath ${BASH_SOURCE[0]})"

    # Export all required variables so they're inherited by the su subshell
    export RESULTS_DIR MAKE_TEST_COMMAND WHPG_MAJORVERSION WHPG_SRC

    su gpadmin -c "bash ${SCRIPT_PATH}"
else
    _main "$@"
fi
