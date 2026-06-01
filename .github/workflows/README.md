# CI Workflows

[![WarehousePG CI](https://github.com/warehouse-pg/warehouse-pg/actions/workflows/whpg-ci.yml/badge.svg)](https://github.com/warehouse-pg/warehouse-pg/actions/workflows/whpg-ci.yml)

## Overview

This directory contains GitHub Actions workflows for the Warehouse-PG project.

## Workflows

### WarehousePG CI (`whpg-ci.yml`)

Main CI workflow for **WHPG 6** that runs regression tests and ORCA unit tests.
`WHPG_MAJORVERSION` is hardcoded to `6`; there is no version detection step.

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
| EL version | `all`, `8` | `8` |
| Debug on failure | `true`, `false` | `false` |

> **Note:** Only EL8 is tested by this workflow. EL9 is intentionally not in
> the matrix because the WHPG 6 build requires Python 2 (PyGreSQL 4.0,
> gpdemo, and the xerces-c 3.1 build script are all Python-2-only), and
> Python 2 is not available on EL9. EL9 build/package coverage is handled
> separately in the `warehouse-pg-packaging` pipeline, so we do not
> duplicate it here.

#### Jobs

| Job | Description | Timeout |
|-----|-------------|---------|
| `detect-config` | Determines EL versions matrix and installcheck target | - |
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
  EL_VERSIONS: '["8"]'                               # EL8 only — WHPG 6 needs Python 2, which EL9 does not ship
  DEFAULT_EL_VERSION: '["8"]'                        # Default for feature branches
  DEFAULT_INSTALLCHECK_TARGET: 'installcheck-small'  # Default installcheck target
```

#### Debugging

When `debug_enabled` is checked during manual dispatch, failed jobs will start a tmate session (30 min timeout) for interactive debugging.

> **Note:** The workflow manually installs tmate via `yum` because `action-tmate` uses `apt-get` internally, which doesn't work on Rocky Linux containers.

## Scripts

Supporting scripts are located in `.github/scripts/`:

| Script | Description |
|--------|-------------|
| `detect-config.bash` | Determines EL versions matrix and installcheck target |
| `run-installcheck.bash` | Runs installcheck tests with proper environment setup |
| `run-orca-tests.bash` | Runs ORCA unit tests using concourse scripts |

### Environment Setup

Scripts source the required environment explicitly rather than relying on `.bash_profile`:

- `run-installcheck.bash` sources `greenplum_path.sh` and `gpdemo-env.sh` directly
- Workflow variables (`WHPG_SRC`, `WHPG_MAJORVERSION`, etc.) are passed via `export` + `su gpadmin` (non-login shell)

### Intentionally Skipped Tests (CI only)

A small number of `isolation2` tests are intentionally skipped on CI. The
skip is applied to the runner's copy of `src/test/isolation2/isolation2_schedule`
by the `apply_ci_test_skips()` function in `.github/scripts/run-installcheck.bash`
(commented out with a `# CI-SKIP:` prefix). The in-repo schedule file is **not**
modified, so a local `make installcheck-world` continues to run every test.

| Test | Reason for CI skip |
|------|--------------------|
| `fts_segment_reset` | FTS (fault tolerance service) timing is flaky on shared ephemeral GitHub Actions runners — the test depends on heartbeat intervals and probe timing that the runner does not deliver reliably, producing false failures. |
| `pg_rewind_fail_missing_xlog` | Exercises a forced-failure path in `pg_rewind` that is sensitive to xlog/WAL filesystem behavior; on the CI container's overlay filesystem the expected error condition does not reproduce deterministically, so the diff is noise rather than a real regression. |

To add or remove a CI-only skip, edit the `skips=( ... )` array in
`apply_ci_test_skips()` in `.github/scripts/run-installcheck.bash`. Do **not**
comment out tests in the schedule file directly — that would also disable them
for local developers.

## Container Images

Tests run in pre-built container images from `ghcr.io/warehouse-pg/`, selected per matrix EL version:

| Image | EL |
|-------|----|
| `ghcr.io/warehouse-pg/whpg7-rocky8-build` | 8 |

> **Note:** The image name retains the `whpg7-` prefix even though this workflow targets WHPG 6 — the same Rocky Linux base image is reused, with WHPG 6-specific packages (xerces-c 3.1, python2-devel) installed at job time.
>
> **EL9:** Not in the matrix. WHPG 6 requires Python 2 (for PyGreSQL 4.0, gpdemo, and the xerces-c 3.1 build script), and Python 2 is not available on EL9, so this workflow cannot build WHPG 6 there. EL9 coverage is provided by the `warehouse-pg-packaging` pipeline instead.

## WHPG 6 Build Configuration

### Xerces-c 3.1

ORCA on WHPG 6 requires xerces-c **3.1**, but the distro ships 3.2. The `Install required packages` step removes the system `xerces-c` / `xerces-c-devel`, and the `Build Xerces with Python 2` step builds 3.1 from `src/backend/gporca/concourse/xerces-c` (a Python-2-only build script, which is why `python2-devel` is installed).

### Dual Python: Python 2 build + Python 3 PL/Python

The workflow runs `configure` twice:

1. **First pass — Python 2** (`Configure` step): builds the full tree against Python 2. WHPG 6 ships PyGreSQL 4.0 (Python-2-only) and the gpdemo / cluster-setup scripts assume `python` is Python 2, so the initial `make` + `make install` must use Python 2.
2. **Second pass — Python 3** (`Rebuild PL/Python with Python 3` step): re-runs `configure` with `PYTHON=/usr/bin/python3`, then `make clean && make install` only inside `src/pl/plpython`. The final install therefore has a Python-2 server with a Python 3 PL/Python module, which the regression tests exercise.

The `python` alternative is flipped accordingly:
- `Set up Python 2 for initial build` — `alternatives --set python /usr/bin/python2` before `configure` and `make_cluster`.
- `Set up Python 3 for plpython test cases` — `alternatives --set python /usr/bin/python3` before `installcheck`.