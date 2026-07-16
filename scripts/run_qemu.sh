#!/usr/bin/env bash
# Deprecated thin wrapper — prefer ./scripts/cgrtos.sh
#   ./scripts/run_qemu.sh            → cgrtos.sh run --app test
#   ./scripts/run_qemu.sh --demo     → cgrtos.sh run --app demo
#   ./scripts/run_qemu.sh --bench    → cgrtos.sh run --app bench
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARGS=()
APP=test
while [[ $# -gt 0 ]]; do
  case "$1" in
    --bench) APP=bench; shift ;;
    --demo) APP=demo; shift ;;
    --test) APP=test; shift ;;
    --no-build) ARGS+=(--no-build); shift ;;
    --timeout) ARGS+=(--timeout "$2"); shift 2 ;;
    --gdb|-g) ARGS+=(--gdb); shift ;;
    -h|--help)
      echo "Deprecated. Use: ./scripts/cgrtos.sh test|demo|bench|cli|..."
      exit 0
      ;;
    *) ARGS+=("$1"); shift ;;
  esac
done
exec "$ROOT/scripts/cgrtos.sh" run --app "${APP}" "${ARGS[@]}"
