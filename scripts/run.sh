#!/usr/bin/env bash
# Run only — requires an existing cgrtos.bin (never runs make).
# Usage: ./scripts/run.sh [--app test] [--cores 2] [--timeout 180] [--gdb] ...
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec "$ROOT/scripts/cgrtos.sh" run "$@"
