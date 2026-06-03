# CI Workflows

[![WarehousePG CI](https://github.com/warehouse-pg/warehouse-pg/actions/workflows/whpg-ci.yml/badge.svg)](https://github.com/warehouse-pg/warehouse-pg/actions/workflows/whpg-ci.yml)

## Overview

This directory contains GitHub Actions workflows for the Warehouse-PG project.

## Workflows

### WarehousePG CI (`whpg-ci.yml`)

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
| `ci/**` (push) | All (installcheck + orca-unit-tests) | Default only | `installcheck-good` |
| PRs targeting `main` / `WHPG_*_STABLE` | All (installcheck + orca-unit-tests) | Default only | `installcheck-good` |

> **Note:** Regular feature branch pushes (e.g., `feature/xyz`) do not trigger CI. Use `ci/` prefix or open a pull request.

> **Optimizer:** every `installcheck` job runs twice — once with `optimizer=on`
> (GPORCA) and once with `optimizer=off` (the Postgres planner) — via the
> `optimizer` matrix dimension, so both planners are covered on every push and PR.

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
| Installcheck target | `installcheck-small`, `installcheck-good`, `installcheck-world` | `installcheck-small` |
| EL version | `all`, `7`, `8`, `9` | `8` |
| Debug on failure | `true`, `false` | `false` |

#### Jobs

| Job | Description | Timeout |
|-----|-------------|---------|
| `installcheck` | Runs regression tests under both optimizers (GPORCA + Postgres planner) | 120 min |
| `orca-unit-tests` | Runs ORCA optimizer unit tests (see below) | 60 min |

**ORCA Unit Tests Details:**

The ORCA unit tests run twice with different build configurations:
1. **RelWithDebInfo** - Release build with debug info (optimized, for performance validation)
2. **Debug** - Debug build (unoptimized, for thorough assertion checking)

This dual-build approach ensures the optimizer works correctly in both production-like and debug environments. The underlying script (`concourse/scripts/unit_tests_gporca.bash`) handles both builds automatically.

#### Configuration

This branch is WHPG 7, so the major version is fixed at `7` and the test
configuration is computed inline in each job — there is no `detect-config`
job. (GitHub Actions matrices cannot read `env`, so the EL lists are written
as literals in the `matrix` / `env` expressions.)

| Trigger | EL versions | Installcheck target |
|---------|-------------|---------------------|
| push to `main` / `WHPG_*_STABLE` | `8`, `9` | `installcheck-world` |
| PRs and `ci/**` pushes | `8` | `installcheck-good` |
| `workflow_dispatch` | per `el_version` input | per `installcheck_target` input |

Every `installcheck` run is multiplied by the `optimizer` matrix
(`on` = GPORCA, `off` = Postgres planner), so both query optimizers are
exercised on every push and PR.

#### Debugging

When `debug_enabled` is checked during manual dispatch, failed jobs will start a tmate session (30 min timeout) for interactive debugging.

> **Note:** The workflow manually installs tmate via `yum` because `action-tmate` uses `apt-get` internally, which doesn't work on Rocky Linux containers.

## Scripts

Supporting scripts are located in `.github/scripts/`:

| Script | Description |
|--------|-------------|
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
| `whpg-rocky{el}-build` | `whpg-rocky8-build` |

The image is selected per matrix EL version; the WHPG major version is fixed at `7`.

## WHPG Version

This branch builds WHPG **7**. The major version is hardcoded in the workflow
(`WHPG_MAJORVERSION: '7'` and the `whpg-rocky{el}-build` image); it is no
longer derived from git tags at runtime.
