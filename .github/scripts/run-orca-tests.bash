#!/bin/bash -l

# Run ORCA unit tests for WHPG
# Wrapper around concourse/scripts/unit_tests_gporca.bash

set -eox pipefail

# Required configuration (from workflow env)
: "${GPDB_SRC:?GPDB_SRC not set}"
: "${EL_VERSION:?EL_VERSION not set}"
: "${WHPG_MAJOR:?WHPG_MAJOR not set}"

function _main() {
    echo "========================================================================"
    echo "Running ORCA unit tests"
    echo "WHPG_MAJOR: ${WHPG_MAJOR}"
    echo "EL_VERSION: ${EL_VERSION}"
    echo "GPDB_SRC: ${GPDB_SRC}"
    echo "========================================================================"

    # Set build architecture
    export BLD_ARCH="rhel${EL_VERSION}_x86_64"
    export GPDB_SRC_PATH="${GPDB_SRC}"

    # Create gpdb_src symlink expected by the script
    if [[ ! -e "${GPDB_SRC}/gpdb_src" ]]; then
        ln -s . "${GPDB_SRC}/gpdb_src"
    fi

    # Run the concourse script
    cd "${GPDB_SRC}"
    bash concourse/scripts/unit_tests_gporca.bash

    echo "========================================================================"
    echo "ORCA unit tests completed successfully"
    echo "========================================================================"
}

_main "$@"
