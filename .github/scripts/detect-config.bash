#!/bin/bash

# Determine test configuration (EL versions and installcheck target)
# Outputs configuration values for GitHub Actions

set -eo pipefail

# =============================================================================
# Required inputs (from workflow env)
# =============================================================================
: "${EVENT_NAME:?EVENT_NAME not set}"
: "${BRANCH_NAME:?BRANCH_NAME not set}"
# INPUT_EL_VERSION and INPUT_INSTALLCHECK_TARGET are optional (only set for workflow_dispatch)

# Required configuration (from workflow env)
: "${EL_VERSIONS:?EL_VERSIONS not set}"
: "${DEFAULT_EL_VERSION:?DEFAULT_EL_VERSION not set}"
: "${DEFAULT_INSTALLCHECK_TARGET:?DEFAULT_INSTALLCHECK_TARGET not set}"

# =============================================================================
# Determine EL versions to test
# =============================================================================
determine_el_versions() {
    local el_versions

    if [[ "$EVENT_NAME" == "workflow_dispatch" ]]; then
        if [[ "$INPUT_EL_VERSION" == "all" ]]; then
            el_versions="$EL_VERSIONS"
        else
            el_versions="[\"$INPUT_EL_VERSION\"]"
        fi
    else
        if [[ "$BRANCH_NAME" == "main" || "$BRANCH_NAME" =~ ^WHPG_.*_STABLE$ ]]; then
            el_versions="$EL_VERSIONS"
        else
            el_versions="$DEFAULT_EL_VERSION"
        fi
    fi

    echo "EL versions to test: $el_versions"
    echo "$el_versions"
}

# =============================================================================
# Determine installcheck target
# =============================================================================
determine_installcheck_target() {
    local target="$INPUT_INSTALLCHECK_TARGET"

    if [[ -z "$target" ]]; then
        # For main/stable branches, run full installcheck-world
        # For feature branches, run quick installcheck-small
        if [[ "$BRANCH_NAME" == "main" || "$BRANCH_NAME" =~ ^WHPG_.*_STABLE$ ]]; then
            target="installcheck-world"
        else
            target="$DEFAULT_INSTALLCHECK_TARGET"
        fi
    fi

    echo "Installcheck target: $target"
    echo "$target"
}

# =============================================================================
# Main
# =============================================================================
main() {
    echo "========================================================================"
    echo "Detecting test configuration"
    echo "========================================================================"
    echo "EVENT_NAME: $EVENT_NAME"
    echo "BRANCH_NAME: $BRANCH_NAME"
    echo "INPUT_EL_VERSION: $INPUT_EL_VERSION"
    echo "INPUT_INSTALLCHECK_TARGET: $INPUT_INSTALLCHECK_TARGET"
    echo "========================================================================"

    local el_output
    el_output=$(determine_el_versions)
    local el_versions
    el_versions=$(echo "$el_output" | tail -1)

    local target_output
    target_output=$(determine_installcheck_target)
    local installcheck_target
    installcheck_target=$(echo "$target_output" | tail -1)

    if [[ -n "$GITHUB_OUTPUT" ]]; then
        echo "el_versions=$el_versions" >> "$GITHUB_OUTPUT"
        echo "installcheck_target=$installcheck_target" >> "$GITHUB_OUTPUT"
    fi

    echo "========================================================================"
    echo "Configuration detected:"
    echo "  EL Versions: $el_versions"
    echo "  Installcheck Target: $installcheck_target"
    echo "========================================================================"
}

main "$@"
