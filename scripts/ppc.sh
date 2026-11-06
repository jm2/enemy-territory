#!/usr/bin/env bash
# Run a command in the et-ppc image (gcc-powerpc-linux-gnu, qemu-user-static) with
# the repo at /src, game data at /data and GL headers at /hostinc.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$ROOT/build-ppc"
exec podman run --rm --network=none -v "$ROOT:/src:Z" -v "${ET_DATA:-$HOME/et-data}:/data:ro,z" -v /usr/include/GL:/hostinc/GL:ro -v /usr/include/KHR:/hostinc/KHR:ro \
    -w /src et-ppc "$@"
