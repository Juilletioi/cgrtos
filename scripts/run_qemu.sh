#!/usr/bin/env bash
# Deprecated — prefer ./scripts/run.sh or ./scripts/cgrtos.sh run
# (run never builds; compile first with ./scripts/build.sh)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARGS=()
APP=test
while [[ $# -gt 0 ]]; do
  case "$1" in
    --bench) APP=bench; shift ;;
    --demo) APP=demo; shift ;;
    --test) APP=test; shift ;;
    --no-build) shift ;; # no-op: run never builds
    --timeout) ARGS+=(--timeout "$2"); shift 2 ;;
    --gdb|-g) ARGS+=(--gdb); shift ;;
    -h|--help)
      echo "Deprecated. Use: ./scripts/build.sh && ./scripts/run.sh"
      echo "See docs/SCRIPTS.md"
      exit 0
      ;;
    *) ARGS+=("$1"); shift ;;
  esac
done
exec "$ROOT/scripts/cgrtos.sh" run --app "${APP}" "${ARGS[@]}"
