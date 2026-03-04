# WHPG CI Workflows

## Overview

This directory contains GitHub Actions workflows for the Warehouse-PG project.

## Workflows

### WHPG CI (`whpg-ci.yml`)

Main CI workflow that runs regression tests and ORCA unit tests.

#### Triggers

| Trigger | Behavior |
|---------|----------|
| **Push** | Runs all tests on any branch |
| **Pull Request** | Runs all tests on any PR |
| **Manual Dispatch** | Run specific tests with custom options |

#### Push/PR Behavior

On push or PR, tests run automatically with these defaults:

| Branch Type | Tests | EL Versions | Installcheck Target |
|-------------|-------|-------------|---------------------|
| `main` / `WHPG_*_STABLE` | All (installcheck + orca-unit-tests) | All configured | `installcheck-world` |
| Feature branches | All (installcheck + orca-unit-tests) | Default only | `installcheck-small` |

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
| `orca-unit-tests` | Runs ORCA optimizer unit tests | 60 min |

#### Configuration

Configuration is centralized at the top of the workflow file (single source of truth). Scripts require these values from the workflow environment and will fail if not provided.

```yaml
env:
  WHPG7_EL_VERSIONS: '["8"]'            # WHPG 7 supported EL versions
  WHPG6_EL_VERSIONS: '["7", "8", "9"]'  # WHPG 6 supported EL versions
  DEFAULT_EL_VERSION: '["8"]'           # Default for feature branches
  DEFAULT_INSTALLCHECK_TARGET: 'installcheck-small'  # Default installcheck target
```

#### Debugging

When `debug_enabled` is checked during manual dispatch, failed jobs will start a tmate session (30 min timeout) for interactive debugging.

## Scripts

Supporting scripts are located in `.github/scripts/`:

| Script | Description |
|--------|-------------|
| `detect-config.bash` | Detects WHPG version and determines test configuration |
| `run-installcheck.bash` | Runs installcheck tests with proper environment setup |
| `run-orca-tests.bash` | Runs ORCA unit tests using concourse scripts |

## Container Images

Tests run in pre-built container images from `ghcr.io/warehouse-pg/`:

- `whpg7-rocky8-build` - WHPG 7 on Rocky Linux 8

## Version Detection

WHPG version is detected from git tags using `git describe --tags --abbrev=0`. The major version (first number before the dot) determines which EL versions to test.

Example: Tag `7.2.1` → WHPG major version `7` → Uses `WHPG7_EL_VERSIONS`
