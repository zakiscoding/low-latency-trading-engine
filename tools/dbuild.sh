#!/usr/bin/env bash
# Runs a command inside the lle-dev image with the repo mounted at /src.
# Build dirs live in a named volume (OneDrive-backed folders are slow for builds).
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd -W 2>/dev/null || pwd)"
MSYS_NO_PATHCONV=1 exec docker run --rm ${DOCKER_EXTRA:-} -v "$repo":/src -v lle-build:/build -w /src lle-dev:latest bash -c "$*"
