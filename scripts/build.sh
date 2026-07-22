#!/usr/bin/env bash
# Compile only (never boots QEMU). Thin wrapper around cgrtos.sh build.
# Usage: ./scripts/build.sh [--app test] [--cores 2] [--clean] [--no-compdb] ...
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec "$ROOT/scripts/cgrtos.sh" build "$@"
