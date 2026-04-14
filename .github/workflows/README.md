# WHPG CI Workflows

[![WHPG CI](https://github.com/warehouse-pg/warehouse-pg/actions/workflows/whpg-ci.yml/badge.svg)](https://github.com/warehouse-pg/warehouse-pg/actions/workflows/whpg-ci.yml)

## Overview

This directory contains GitHub Actions workflows for the Warehouse-PG project.

## Workflows

### WHPG CI (`whpg-ci.yml`)

Main CI workflow that runs regression tests and ORCA unit tests.

#### Triggers

| Trigger | Behavior |
|---------|----------|
| **Push** | Runs on `main`, `WHPG_*_STABLE`, and `ci/**` branches |
| **Pull Request** | Runs on PRs targeting `main` and `WHPG_*_STABLE` |
| **Manual Dispatch** | Run specific tests with custom options |

#### Testing on Feature Branches

To run CI on a feature branch without opening a PR, prefix the branch name with `ci/`:

```bash
git checkout -b ci/my-feature    # CI will run on push
git push origin ci/my-feature
```

Regular feature branches (e.g., `feature/xyz`) do not trigger CI to save resources.

#### Push/PR Behavior

On push or PR, tests run automatically with these defaults:

| Branch Type | Tests | EL Versions | Installcheck Target |
|-------------|-------|-------------|---------------------|
| `main` / `WHPG_*_STABLE` (push) | All (installcheck + orca-unit-tests) | All configured | `installcheck-world` |
| `ci/**` (push) | All (installcheck + orca-unit-tests) | Default only | `installcheck-small` |
| PRs targeting `main` / `WHPG_*_STABLE` | All (installcheck + orca-unit-tests) | Default only | `installcheck-small` |

> **Note:** Regular feature branch pushes (e.g., `feature/xyz`) do not trigger CI. Use `ci/` prefix or open a pull request.

#### Concurrency

The workflow uses concurrency groups to manage parallel runs:

```yaml
concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: ${{ github.ref != 'refs/heads/main' && !startsWith(github.ref, 'refs/heads/WHPG_') }}
```

| Branch | Behavior |
|--------|----------|
| `main` / `WHPG_*_STABLE` | All runs complete (no cancellation) |
| PR branches | Older runs cancelled when new commits pushed |

#### Caching

The workflow uses multiple caches to speed up builds:

**ccache (compiler cache)**
- Cache stored per EL version and WHPG version
- First run: full build (~30 min)
- Subsequent runs: incremental build (~5 min)

| Component | Build System | ccache |
|-----------|--------------|--------|
| WHPG (main) | autotools | ✅ `CC='ccache gcc'` |
| ORCA | cmake | ✅ `CMAKE_*_LAUNCHER` |
| xerces | autotools | ✅ `CC/CXX` |

**yum packages**
- Caches downloaded RPM packages
- Key scoped per job, EL version, WHPG version, and workflow file hash
- Reduces package download time on subsequent runs

#### Job Summary

Each job generates a summary (visible in the GitHub Actions Summary tab) showing:
- WHPG and EL versions, test target
- Step-by-step results (✅ passed / ❌ failed / ⏭️ skipped)
- ccache statistics (hits, misses, hit rate, cache size)

Summaries run with `if: always()` so they are available even when jobs fail.

#### Manual Dispatch Behavior

On manual dispatch, you can customize:

| Option | Choices | Default |
|--------|---------|---------|
| Test type | `all`, `installcheck`, `orca-unit-tests` | `all` |
| Installcheck target | `installcheck-small`, `installcheck-world` | `installcheck-small` |
| EL version | `all`, `7`, `8`, `9` | `8` |
| Debug on failure | `true`, `false` | `false` |

#### Jobs

| Job | Description | Timeout |
|-----|-------------|---------|
| `detect-config` | Detects WHPG version and test configuration | - |
| `installcheck` | Runs PostgreSQL regression tests | 120 min |
| `orca-unit-tests` | Runs ORCA optimizer unit tests (see below) | 60 min |

**ORCA Unit Tests Details:**

The ORCA unit tests run twice with different build configurations:
1. **RelWithDebInfo** - Release build with debug info (optimized, for performance validation)
2. **Debug** - Debug build (unoptimized, for thorough assertion checking)

This dual-build approach ensures the optimizer works correctly in both production-like and debug environments. The underlying script (`concourse/scripts/unit_tests_gporca.bash`) handles both builds automatically.

#### Configuration

Configuration is centralized at the top of the workflow file (single source of truth). Scripts require these values from the workflow environment and will fail if not provided.

```yaml
env:
  WHPG7_EL_VERSIONS: '["8","9"]'            # WHPG 7 supported EL versions
  WHPG6_EL_VERSIONS: '["7", "8", "9"]'  # WHPG 6 supported EL versions
  DEFAULT_EL_VERSION: '["8"]'           # Default for feature branches
  DEFAULT_INSTALLCHECK_TARGET: 'installcheck-small'  # Default installcheck target
```

#### Debugging

When `debug_enabled` is checked during manual dispatch, failed jobs will start a tmate session (30 min timeout) for interactive debugging.

> **Note:** The workflow manually installs tmate via `yum` because `action-tmate` uses `apt-get` internally, which doesn't work on Rocky Linux containers.

## Scripts

Supporting scripts are located in `.github/scripts/`:

| Script | Description |
|--------|-------------|
| `detect-config.bash` | Detects WHPG version and determines test configuration |
| `run-installcheck.bash` | Runs installcheck tests with proper environment setup |
| `run-orca-tests.bash` | Runs ORCA unit tests using concourse scripts |

### Environment Setup

Scripts source the required environment explicitly rather than relying on `.bash_profile`:

- `run-installcheck.bash` sources `greenplum_path.sh` and `gpdemo-env.sh` directly
- Workflow variables (`WHPG_SRC`, `WHPG_MAJORVERSION`, etc.) are passed via `export` + `su gpadmin` (non-login shell)

## Container Images

Tests run in pre-built container images from `ghcr.io/warehouse-pg/`:

| Image Pattern | Example |
|---------------|---------|
| `whpg{major}-rocky{el}-build` | `whpg7-rocky8-build` |

The image is automatically selected based on detected WHPG version and matrix EL version.

## Version Detection

WHPG version is detected from git tags using `git describe --tags --abbrev=0`. The major version (first number before the dot) determines which EL versions to test.

Example: Tag `7.2.1` → WHPG major version `7` → Uses `WHPG7_EL_VERSIONS`
