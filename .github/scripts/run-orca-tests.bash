#!/bin/bash

# Run ORCA unit tests for WHPG
# Wrapper around concourse/scripts/unit_tests_gporca.bash

set -eox pipefail

# Required configuration (from workflow env)
: "${WHPG_SRC:?WHPG_SRC not set}"
: "${EL_VERSION:?EL_VERSION not set}"
: "${WHPG_MAJORVERSION:?WHPG_MAJORVERSION not set}"

function _main() {
    echo "========================================================================"
    echo "Running ORCA unit tests"
    echo "WHPG_MAJORVERSION: ${WHPG_MAJORVERSION}"
    echo "EL_VERSION: ${EL_VERSION}"
    echo "WHPG_SRC: ${WHPG_SRC}"
    echo "========================================================================"

    # Source common functions and set build architecture dynamically
    cd "${WHPG_SRC}"
    source concourse/scripts/common.bash
    export BLD_ARCH=$(build_arch)
    export GPDB_SRC_PATH="${WHPG_SRC}"

    echo "BLD_ARCH: ${BLD_ARCH}"

    # Create gpdb_src symlink expected by the script
    if [[ ! -e "${WHPG_SRC}/gpdb_src" ]]; then
        ln -s . "${WHPG_SRC}/gpdb_src"
    fi

    # Run the concourse script
    bash concourse/scripts/unit_tests_gporca.bash

    echo "========================================================================"
    echo "ORCA unit tests completed successfully"
    echo "========================================================================"
}

_main "$@"
