#!/bin/bash
set -exo pipefail

GPDB_SRC_PATH=${GPDB_SRC_PATH:=gpdb_src}

function test_orca
{
    if [ -n "${SKIP_UNITTESTS}" ]; then
        return
    fi
    OUTPUT_DIR="../../../../gpAux/ext/${BLD_ARCH}"
    pushd ${GPDB_SRC_PATH}/src/backend/gporca
    concourse/build_and_test.py --build_type=RelWithDebInfo --output_dir=${OUTPUT_DIR}
    concourse/build_and_test.py --build_type=Debug --output_dir=${OUTPUT_DIR}
    popd
}

function _main
{
  # Install prefix for the ORCA build; WHPG 7 builds and tests ORCA against
  # the distribution's xerces-c-devel, like the RPM does, and vendors nothing.
  mkdir gpdb_src/gpAux/ext
  test_orca
}

_main "$@"
