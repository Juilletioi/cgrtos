#!/usr/bin/env bash
# =============================================================================
# CG-RTOS all-in-one helper: build + QEMU + GDB + docs/sdk
# =============================================================================
#   ./scripts/cgrtos.sh test|cli|stress|demo|bench
#   ./scripts/cgrtos.sh test --gdb
#   ./scripts/cgrtos.sh sdk              # Doxygen + sdk/ package
#   ./scripts/cgrtos.sh help
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SYSROOT="${SYSROOT:-/home/cong/nuclei-ux900fd-linux/nuclei-linux-sdk/work/evalsoc/buildroot_initramfs/host/opt/ext-toolchain}"
QEMU="${QEMU:-/home/cong/nuclei-ux900fd-linux/tools/nuclei-qemu/bin/qemu-system-riscv64}"
GDB="${GDB:-${SYSROOT}/bin/riscv64-unknown-linux-gnu-gdb}"
export PATH="${SYSROOT}/bin:${PATH}"

CMD="${1:-help}"
shift || true

APP=test
NO_BUILD=0
TIMEOUT_SEC="${TIMEOUT_SEC:-120}"
GDB_PORT=1234
WITH_GDB=0
CORES="${CORES:-2}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --app) APP="$2"; shift 2 ;;
    --demo) APP=demo; shift ;;
    --test) APP=test; shift ;;
    --bench) APP=bench; shift ;;
    --stress) APP=stress; shift ;;
    --cli) APP=cli; shift ;;
    --cores) CORES="$2"; shift 2 ;;
    --no-build) NO_BUILD=1; shift ;;
    --timeout) TIMEOUT_SEC="$2"; shift 2 ;;
    --port) GDB_PORT="$2"; shift 2 ;;
    --gdb|-g) WITH_GDB=1; shift ;;
    -h|--help) CMD=help; break ;;
    *) echo "Unknown arg: $1"; exit 2 ;;
  esac
done

case "${CORES}" in
  1|2|4) ;;
  *) echo "ERROR: --cores must be 1, 2, or 4 (got '${CORES}')"; exit 2 ;;
esac

QEMU_MACHINE=(
  -M nuclei_evalsoc,download=ddr
  -smp "${CORES}" -m 512M
  -cpu nuclei-ux900fd,ext=svpbmt_zicbom_sstc_sscofpmf_zba_zbb_zbc_zbs_zicond
  -nographic -serial mon:stdio
)
# Nuclei ROM often boots only hart0; force secondary PCs to DDR image entry
if [[ "${CORES}" -ge 2 ]]; then
  QEMU_MACHINE+=(-device loader,addr=0xA0000000,cpu-num=1)
fi
if [[ "${CORES}" -ge 4 ]]; then
  QEMU_MACHINE+=(-device loader,addr=0xA0000000,cpu-num=2)
  QEMU_MACHINE+=(-device loader,addr=0xA0000000,cpu-num=3)
fi

need_toolchain() {
  if [[ ! -x "${SYSROOT}/bin/riscv64-unknown-linux-gnu-gcc" ]]; then
    echo "ERROR: toolchain not found at ${SYSROOT}/bin"
    exit 1
  fi
}

need_qemu() {
  if [[ ! -x "${QEMU}" ]]; then
    echo "ERROR: QEMU not found at ${QEMU}"
    exit 1
  fi
}

need_gdb() {
  if [[ ! -x "${GDB}" ]]; then
    GDB="$(command -v riscv64-unknown-linux-gnu-gdb || true)"
  fi
  if [[ -z "${GDB}" || ! -x "${GDB}" ]]; then
    echo "ERROR: GDB not found (set GDB=...)"
    exit 1
  fi
}

do_build() {
  need_toolchain
  echo "==> Building APP=${APP} CORES=${CORES}"
  make clean
  make all APP="${APP}" CORES="${CORES}"
  echo "==> Built cgrtos.elf / cgrtos.bin (CONFIG_NUM_CORES=${CORES})"
}

ensure_built() {
  if [[ "${NO_BUILD}" -eq 0 ]]; then
    do_build
  elif [[ ! -f cgrtos.bin ]]; then
    echo "ERROR: cgrtos.bin missing — build first"
    exit 1
  fi
  if [[ "${WITH_GDB}" -eq 1 && ! -f cgrtos.elf ]]; then
    echo "ERROR: cgrtos.elf missing (needed for GDB symbols)"
    exit 1
  fi
}

# ---------------------------------------------------------------------------
# Open GDB (+ TUI C source) in a second window / pane.
# Prefers: Windows Terminal (WSL) → tmux split → GUI terminal → same tty.
# ---------------------------------------------------------------------------
open_gdb_window() {
  need_gdb
  local gdb_script
  gdb_script="$(mktemp /tmp/cgrtos-gdb.XXXXXX.sh)"
  cat >"${gdb_script}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
cd $(printf '%q' "${ROOT}")
export PATH=$(printf '%q' "${PATH}")
# Give QEMU a moment to bind the gdbstub
sleep 1
exec $(printf '%q' "${GDB}") -q -tui \\
  -ex $(printf '%q' "target remote :${GDB_PORT}") \\
  -x $(printf '%q' "${ROOT}/scripts/gdbinit") \\
  $(printf '%q' "${ROOT}/cgrtos.elf")
EOF
  chmod +x "${gdb_script}"

  # Allow override: CGRTOS_GDB_TERM='wt' | 'tmux' | 'xterm' | 'here'
  local prefer="${CGRTOS_GDB_TERM:-auto}"
  local launched=0

  try_wt() {
    local wt=""
    local candidates=(
      "${CGRTOS_WT:-}"
      "$(command -v wt.exe 2>/dev/null || true)"
    )
    local c
    # Resolve WindowsApps execution alias / package install of Windows Terminal
    shopt -s nullglob
    candidates+=(
      /mnt/c/Users/*/AppData/Local/Microsoft/WindowsApps/Microsoft.WindowsTerminal_*/wt.exe
      /mnt/c/Users/*/AppData/Local/Microsoft/WindowsApps/wt.exe
    )
    shopt -u nullglob
    for c in "${candidates[@]}"; do
      [[ -n "${c}" && -e "${c}" ]] || continue
      wt="${c}"
      break
    done
    [[ -n "${wt}" && -n "${WSL_DISTRO_NAME:-}" ]] || return 1
    # New Windows Terminal tab running GDB inside this WSL distro
    "${wt}" -w 0 new-tab --title "CG-RTOS GDB (${APP})" \
      wsl.exe -d "${WSL_DISTRO_NAME}" --cd "${ROOT}" \
      -e bash "${gdb_script}" >/dev/null 2>&1 &
    return 0
  }

  try_tmux() {
    [[ -n "${TMUX:-}" ]] || return 1
    tmux split-window -h "bash '${gdb_script}'; tmux wait-for -S cgrtos-gdb-done"
    return 0
  }

  try_xterm() {
    local term=""
    for cand in gnome-terminal xfce4-terminal mate-terminal x-terminal-emulator xterm; do
      if command -v "${cand}" >/dev/null 2>&1; then
        term="${cand}"
        break
      fi
    done
    [[ -n "${term}" ]] || return 1
    case "${term}" in
      gnome-terminal|xfce4-terminal|mate-terminal)
        "${term}" --title="CG-RTOS GDB (${APP})" -- bash "${gdb_script}" &
        ;;
      *)
        "${term}" -T "CG-RTOS GDB (${APP})" -e "bash ${gdb_script}" &
        ;;
    esac
    return 0
  }

  case "${prefer}" in
    wt) try_wt && launched=1 ;;
    tmux) try_tmux && launched=1 ;;
    xterm|gui) try_xterm && launched=1 ;;
    here) launched=0 ;;
    auto)
      if try_wt || try_tmux || try_xterm; then
        launched=1
      fi
      ;;
  esac

  if [[ "${launched}" -eq 1 ]]; then
    echo "==> GDB opened in another window/pane (TUI source view)"
    echo "    Port :${GDB_PORT}  ELF ${ROOT}/cgrtos.elf"
    # Keep script path for the child; remove later on exit
    GDB_LAUNCH_SCRIPT="${gdb_script}"
    return 0
  fi

  # Fallback: GDB takes over this tty after QEMU is backgrounded by caller
  echo "==> No external terminal found — GDB will use this window"
  echo "    Tip (WSL): install Windows Terminal, or run inside tmux"
  echo "    Or set CGRTOS_GDB_TERM=here and use two terminals manually"
  GDB_LAUNCH_SCRIPT="${gdb_script}"
  return 1
}

do_gdb_session() {
  need_qemu
  ensure_built
  need_gdb

  echo "==> Debug session APP=${APP}"
  echo "    This window : QEMU UART (Ctrl-A X to quit)"
  echo "    Other window: GDB + C source (TUI)"
  echo ""

  local gdb_here=0
  if ! open_gdb_window; then
    gdb_here=1
  fi

  cleanup_gdb_session() {
    if [[ -n "${QEMU_PID:-}" ]]; then
      kill "${QEMU_PID}" 2>/dev/null || true
      wait "${QEMU_PID}" 2>/dev/null || true
    fi
    if [[ -n "${GDB_LAUNCH_SCRIPT:-}" ]]; then
      rm -f "${GDB_LAUNCH_SCRIPT}"
    fi
  }
  trap cleanup_gdb_session EXIT

  if [[ "${gdb_here}" -eq 1 ]]; then
    # Same-window fallback: QEMU background, GDB foreground
    "${QEMU}" "${QEMU_MACHINE[@]}" -bios cgrtos.bin \
      -gdb tcp::"${GDB_PORT}" -S &
    QEMU_PID=$!
    sleep 0.5
    bash "${GDB_LAUNCH_SCRIPT}" || true
    return 0
  fi

  # Two-window: QEMU owns this terminal (UART + stdin for CLI)
  "${QEMU}" "${QEMU_MACHINE[@]}" -bios cgrtos.bin \
    -gdb tcp::"${GDB_PORT}" -S
}

do_run() {
  if [[ "${WITH_GDB}" -eq 1 ]]; then
    do_gdb_session
    return
  fi

  need_qemu
  ensure_built

  LOG="$(mktemp /tmp/cgrtos-qemu.XXXXXX.log)"
  trap 'rm -f "${LOG}"' EXIT

  echo "==> QEMU APP=${APP} CORES=${CORES} timeout=${TIMEOUT_SEC}s"
  set +e
  timeout "${TIMEOUT_SEC}" "${QEMU}" "${QEMU_MACHINE[@]}" -bios cgrtos.bin >"${LOG}" 2>&1
  QEMU_RC=$?
  set -e

  OUT="$(tr -d '\r' <"${LOG}")"
  printf '%s\n' "${OUT}"
  echo ""
  echo "==> Summary"
  PASS_N="$(printf '%s\n' "${OUT}" | grep -c '\[PASS\]' || true)"
  FAIL_N="$(printf '%s\n' "${OUT}" | grep -c '\[FAIL\]' || true)"
  BENCH_N="$(printf '%s\n' "${OUT}" | grep -c '^BENCH ' || true)"
  echo "  [PASS]=${PASS_N}  [FAIL]=${FAIL_N}  BENCH=${BENCH_N}"

  case "${APP}" in
    bench)
      if printf '%s\n' "${OUT}" | grep -q '=== BENCH_DONE ==='; then
        echo "---- Bench ----"
        printf '%s\n' "${OUT}" | grep '^BENCH ' || true
        echo "RESULT: BENCH_DONE"; exit 0
      fi
      echo "RESULT: BENCH_INCOMPLETE"; exit 1
      ;;
    stress)
      if printf '%s\n' "${OUT}" | grep -q '=== STRESS_PASSED ==='; then
        echo "RESULT: STRESS_PASSED"; exit 0
      fi
      if printf '%s\n' "${OUT}" | grep -q '=== STRESS_FAILED ==='; then
        echo "RESULT: STRESS_FAILED"; exit 1
      fi
      if [[ "${QEMU_RC}" -eq 124 ]]; then
        echo "RESULT: TIMEOUT"; exit 1
      fi
      echo "RESULT: STRESS_INCOMPLETE (qemu rc=${QEMU_RC})"; exit 1
      ;;
    demo)
      if printf '%s\n' "${OUT}" | grep -q '\[T1\] LED OFF'; then
        echo "RESULT: DEMO_OK"; exit 0
      fi
      echo "RESULT: DEMO_INCOMPLETE"; exit 1
      ;;
    *)
      if printf '%s\n' "${OUT}" | grep -q '=== TEST_SUITE_PASSED ==='; then
        echo "RESULT: TEST_SUITE_PASSED"; exit 0
      fi
      # Suite may print Results before the marker; accept that too
      if printf '%s\n' "${OUT}" | grep -qE 'Results: [0-9]+ passed, 0 failed'; then
        echo "RESULT: TEST_SUITE_PASSED"; exit 0
      fi
      if printf '%s\n' "${OUT}" | grep -q '=== TEST_SUITE_FAILED ==='; then
        echo "RESULT: TEST_SUITE_FAILED"; exit 1
      fi
      if [[ "${QEMU_RC}" -eq 124 ]]; then
        echo "RESULT: TIMEOUT"; exit 1
      fi
      echo "RESULT: NO_SUITE_MARKER (qemu rc=${QEMU_RC})"; exit 1
      ;;
  esac
}

do_gdb() {
  # Legacy entry: ./scripts/cgrtos.sh gdb [--app X]
  WITH_GDB=1
  do_gdb_session
}

do_cli() {
  APP=cli
  if [[ "${WITH_GDB}" -eq 1 ]]; then
    do_gdb_session
    return
  fi

  need_qemu
  ensure_built

  echo "==> Interactive CLI (Ctrl-A X to quit QEMU)"
  echo "    Commands: help | list | run <case>|all|stress | stats | heap | cores"
  echo "    Debug:    ./scripts/cgrtos.sh cli --gdb"
  echo ""
  # Keep stdin/stdout attached — do not capture or timeout.
  exec "${QEMU}" "${QEMU_MACHINE[@]}" -bios cgrtos.bin
}

do_docs() {
  if ! command -v doxygen >/dev/null 2>&1; then
    echo "ERROR: doxygen not installed. Try: sudo apt install doxygen graphviz"
    exit 1
  fi
  echo "==> Generating Doxygen HTML -> docs/doxygen/html/"
  rm -rf "${ROOT}/docs/doxygen"
  (cd "${ROOT}" && doxygen Doxyfile)
  local index="${ROOT}/docs/doxygen/html/index.html"
  if [[ ! -f "${index}" ]]; then
    echo "ERROR: Doxygen failed (missing ${index})"
    exit 1
  fi
  echo "==> Done: ${index}"
  echo "    Open with: xdg-open ${index}   # or browse file://${index}"
}

# Package a minimal application SDK: public header + API HTML docs.
do_sdk() {
  do_docs
  local sdk="${ROOT}/sdk"
  echo "==> Packaging SDK -> ${sdk}/"
  rm -rf "${sdk}"
  mkdir -p "${sdk}/include" "${sdk}/include/hal" "${sdk}/docs"
  cp -a "${ROOT}/kernel/cgrtos.h" "${sdk}/include/cgrtos.h"
  cp -a "${ROOT}/kernel/list.h" "${sdk}/include/list.h"
  cp -a "${ROOT}/hal/hal.h" "${sdk}/include/hal/hal.h"
  cp -a "${ROOT}/hal/hal_board.h" "${sdk}/include/hal/hal_board.h"
  # Lightweight HTML tree copy (already generated under docs/doxygen)
  cp -a "${ROOT}/docs/doxygen/html" "${sdk}/docs/api"
  cat >"${sdk}/README.md" <<'EOF'
# CG-RTOS Application SDK

本目录由 `./scripts/cgrtos.sh sdk` 生成，供应用侧引用公开 API。

## 内容

| 路径 | 说明 |
|------|------|
| `include/cgrtos.h` | 内核公共 API（会 `#include "hal/hal.h"`） |
| `include/hal/hal.h` | 统一驱动框架 + 用户 HAL API |
| `include/hal/hal_board.h` | Nuclei evalsoc 板级 MMIO |
| `include/list.h` | `cgrtos.h` 依赖 |
| `docs/api/index.html` | Doxygen API 文档（浏览器打开） |

## 使用

```c
#include "cgrtos.h"
/* 新代码也可用：#include "hal/hal.h" */

void demo(void) {
    hal_console_puts("hello HAL\n");
    cgrtos_printf("mtime=%lx\n", (unsigned long)hal_mtime_read());
}
```

编译时增加 `-I<path-to-sdk>/include`，并链入本仓库构建的内核目标（见仓库根 `Makefile` / `docs/USER_GUIDE.md`）。

完整使用指南：仓库 `docs/USER_GUIDE.md`。
EOF
  echo "==> SDK ready: ${sdk}/include/cgrtos.h"
  echo "               ${sdk}/include/hal/hal.h"
  echo "               ${sdk}/docs/api/index.html"
}

print_help() {
  cat <<'EOF'
CG-RTOS helper — build / run / debug / docs / sdk

Usage:
  ./scripts/cgrtos.sh <command> [options]

Commands:
  build     Compile only (--app demo|test|bench|stress|cli)
  run       Build (unless --no-build) and run in QEMU
  test      Feature suite (APP=test) → expect TEST_SUITE_PASSED
  demo      Blinky / IPC demo
  bench     Microbenchmarks (default timeout 30s)
  stress    Concurrent stress (default timeout 60s; not in run all)
  cli       Interactive UART CLI (list / run <case>)
  gdb       Same as <app> --gdb (default APP=test)
  docs      Regenerate Doxygen HTML → docs/doxygen/html/
  sdk       docs + package sdk/ (include/cgrtos.h + docs/api/)
  help      This message

Options:
  --app NAME     demo | test | bench | stress | cli
  --cores N      Hart count: 1 | 2 | 4 (default 2; sets -DCONFIG_NUM_CORES and QEMU -smp)
  --gdb | -g     This window = QEMU UART; other window = GDB TUI (C source)
  --no-build     Skip make; reuse existing binary
  --timeout SEC  QEMU wall timeout (default 120; stress 60; ignored with --gdb)
  --port N       GDB stub port (default 1234)

Examples:
  ./scripts/cgrtos.sh test
  ./scripts/cgrtos.sh test --cores 1
  ./scripts/cgrtos.sh test --cores 4
  ./scripts/cgrtos.sh cli                 # then: list / run streambuf / run fs
  ./scripts/cgrtos.sh test --gdb
  ./scripts/cgrtos.sh sdk                 # refresh API SDK + Doxygen

Environment:
  SYSROOT          RISC-V toolchain prefix
  QEMU             qemu-system-riscv64 path
  GDB              gdb path
  CORES            default hart count if --cores omitted (1|2|4)
  CGRTOS_GDB_TERM  auto|wt|tmux|xterm|here

See docs/USER_GUIDE.md for the full guide.
EOF
}

case "${CMD}" in
  help|-h|--help) print_help ;;
  build) do_build ;;
  run) do_run ;;
  test) APP=test; do_run ;;
  demo) APP=demo; do_run ;;
  bench) APP=bench; TIMEOUT_SEC="${TIMEOUT_SEC:-30}"; do_run ;;
  stress) APP=stress; TIMEOUT_SEC="${TIMEOUT_SEC:-60}"; do_run ;;
  cli) do_cli ;;
  gdb) do_gdb ;;
  docs) do_docs ;;
  sdk) do_sdk ;;
  *) echo "Unknown command: ${CMD}"; print_help; exit 2 ;;
esac
