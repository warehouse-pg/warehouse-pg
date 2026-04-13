#!/bin/bash

# Detect WHPG version and test configuration
# Outputs configuration values for GitHub Actions

set -eo pipefail

# =============================================================================
# Required inputs (from workflow env)
# =============================================================================
: "${EVENT_NAME:?EVENT_NAME not set}"
: "${BRANCH_NAME:?BRANCH_NAME not set}"
# INPUT_EL_VERSION and INPUT_INSTALLCHECK_TARGET are optional (only set for workflow_dispatch)

# Required configuration (from workflow env)
: "${WHPG7_EL_VERSIONS:?WHPG7_EL_VERSIONS not set}"
: "${WHPG6_EL_VERSIONS:?WHPG6_EL_VERSIONS not set}"
: "${DEFAULT_EL_VERSION:?DEFAULT_EL_VERSION not set}"
: "${DEFAULT_INSTALLCHECK_TARGET:?DEFAULT_INSTALLCHECK_TARGET not set}"

# =============================================================================
# Detect WHPG version from git tags
# =============================================================================
detect_whpg_version() {
    local tag
    tag=$(git describe --tags --abbrev=0 2>/dev/null || echo "")

    if [[ -z "$tag" ]]; then
        echo "::error::No git tag found. Cannot determine WHPG version." >&2
        exit 1
    fi

    # Extract major version (first number before dot)
    local ver
    ver=$(echo "$tag" | cut -d'.' -f1)

    if [[ ! "$ver" =~ ^[0-9]+$ ]]; then
        echo "::error::Extracted version '$ver' from tag '$tag' is not numeric." >&2
        exit 1
    fi

    echo "Found tag: $tag"
    echo "Detected WHPG major version: $ver"
    echo "$ver"
}

# =============================================================================
# Determine EL versions to test
# =============================================================================
determine_el_versions() {
    local whpg_major="$1"
    local el_versions

    # Validate WHPG version
    if [[ "$whpg_major" != "7" && "$whpg_major" != "6" ]]; then
        echo "::error::Unsupported WHPG major version: $whpg_major. Only 6 and 7 are supported." >&2
        exit 1
    fi

    if [[ "$EVENT_NAME" == "workflow_dispatch" ]]; then
        # Dispatch: user-selected el_version
        if [[ "$INPUT_EL_VERSION" == "all" ]]; then
            # "all" selected - expand based on WHPG version
            if [[ "$whpg_major" == "7" ]]; then
                el_versions="$WHPG7_EL_VERSIONS"
            else
                el_versions="$WHPG6_EL_VERSIONS"
            fi
        else
            # Specific version selected
            el_versions="[\"$INPUT_EL_VERSION\"]"
        fi
    else
        # Push/PR: check if main/stable branch for full matrix
        if [[ "$BRANCH_NAME" == "main" || "$BRANCH_NAME" =~ ^WHPG_.*_STABLE$ ]]; then
            # Main/stable branch: run all EL versions
            if [[ "$whpg_major" == "7" ]]; then
                el_versions="$WHPG7_EL_VERSIONS"
            else
                el_versions="$WHPG6_EL_VERSIONS"
            fi
        else
            # Feature branch: run default EL version only
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
    echo "Detecting WHPG version and test configuration"
    echo "========================================================================"
    echo "EVENT_NAME: $EVENT_NAME"
    echo "BRANCH_NAME: $BRANCH_NAME"
    echo "INPUT_EL_VERSION: $INPUT_EL_VERSION"
    echo "INPUT_INSTALLCHECK_TARGET: $INPUT_INSTALLCHECK_TARGET"
    echo "========================================================================"

    # Detect WHPG version
    local whpg_output
    whpg_output=$(detect_whpg_version)
    local whpg_major
    whpg_major=$(echo "$whpg_output" | tail -1)

    # Determine EL versions
    local el_output
    el_output=$(determine_el_versions "$whpg_major")
    local el_versions
    el_versions=$(echo "$el_output" | tail -1)

    # Determine installcheck target
    local target_output
    target_output=$(determine_installcheck_target)
    local installcheck_target
    installcheck_target=$(echo "$target_output" | tail -1)

    # Output to GitHub Actions
    if [[ -n "$GITHUB_OUTPUT" ]]; then
        echo "ver=$whpg_major" >> "$GITHUB_OUTPUT"
        echo "el_versions=$el_versions" >> "$GITHUB_OUTPUT"
        echo "installcheck_target=$installcheck_target" >> "$GITHUB_OUTPUT"
    fi

    echo "========================================================================"
    echo "Configuration detected:"
    echo "  WHPG Major: $whpg_major"
    echo "  EL Versions: $el_versions"
    echo "  Installcheck Target: $installcheck_target"
    echo "========================================================================"
}

main "$@"
