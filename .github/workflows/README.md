# GitHub Actions Workflows

This directory contains CI/CD workflows for the gst-tsfilter-plugin project.

## Workflows

### CI Workflow (`ci.yml`)

**Triggers:**
- Push to `main` or `develop` branches
- Pull requests to `main` or `develop`
- Manual workflow dispatch

**Jobs:**

1. **Docker CI (Ubuntu 24.04)**
   - Builds Docker image using `docker/build.sh`
   - Runs C test suite (42 tests)
   - Runs quick test (20 files)
   - Runs full test suite (281 files) - only on pushes to `main` branch

2. **Native Build (Ubuntu Latest)**
   - Installs GStreamer dependencies
   - Builds with CMake
   - Runs C test suite
   - Verifies plugin loads

3. **Meson Build**
   - Builds only the plugin with Meson
   - Verifies plugin loads

### Docker Build Workflow (`docker-publish.yml`)

**Triggers:**
- Push to `main` branch
- Version tags (e.g., `v1.0.0`)
- Pull requests
- Manual workflow dispatch

**What it does:**
- Builds Docker image using `docker/Dockerfile`
- Verifies image builds successfully (no push to registry)

## Status Badges

Add this to your README.md:

```markdown
[![CI](https://github.com/larixsoft/gst-tsfilter-plugin/actions/workflows/ci.yml/badge.svg)](https://github.com/larixsoft/gst-tsfilter-plugin/actions/workflows/ci.yml)
```

## Local Testing of Workflows

Use [act](https://github.com/nektos/act) to test GitHub Actions locally:

```bash
# Install act
brew install act  # macOS
# or
curl https://raw.githubusercontent.com/nektos/act/master/install.sh | sudo bash

# Run workflows
act push
act -j native-build
```

## Secrets

No additional secrets required for CI. The workflow uses:
- `GITHUB_TOKEN` (automatic) - for publishing to ghcr.io

## Caching

- Docker layers are cached using GitHub Actions cache
- CMake and Meson build directories are cached automatically
