#!/usr/bin/env bash
# =============================================================================
# CG-RTOS — 编译 / 运行 / clangd / 文档 统一入口（多架构）
# =============================================================================
# 设计原则：编译与运行分离。
#   build   → 只编译（可选生成 compile_commands.json）
#   run     → 只运行已有镜像（不自动 make）
#   test/…  → 先 build 再 run（可用 --no-build）
#
# 详细说明见：docs/SCRIPTS.md
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# RISC-V 默认工具链根；ARM64 使用 cgrtos-tools 或系统 aarch64-linux-gnu-*
SYSROOT="${SYSROOT:-/home/cong/nuclei-ux900fd-linux/nuclei-linux-sdk/work/evalsoc/buildroot_initramfs/host/opt/ext-toolchain}"
TOOLS_ROOT="${TOOLS_ROOT:-/home/cong/cgrtos-tools/root}"
GDB="${GDB:-}"
QEMU_OVERRIDE="${QEMU:-}"
QEMU=""
BOARD_ARCH="riscv"
CROSS_PREFIX="riscv64-unknown-linux-gnu-"

CMD="${1:-help}"
shift || true

APP="${APP:-test}"
NO_BUILD=0
DO_CLEAN=0
DO_COMPDB=1
TIMEOUT_SEC="${TIMEOUT_SEC:-120}"
GDB_PORT=1234
WITH_GDB=0
CORES="${CORES:-2}"
BOARD="${BOARD:-nuclei_evalsoc}"
CPU="${CPU:-}"
PROFILE="${PROFILE:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --app) APP="$2"; shift 2 ;;
    --demo) APP=demo; shift ;;
    --test) APP=test; shift ;;
    --bench) APP=bench; shift ;;
    --stress) APP=stress; shift ;;
    --cli) APP=cli; shift ;;
    --cores) CORES="$2"; shift 2 ;;
    --board) BOARD="$2"; shift 2 ;;
    --cpu) CPU="$2"; shift 2 ;;
    --profile) PROFILE="$2"; shift 2 ;;
    --no-build) NO_BUILD=1; shift ;;
    --clean) DO_CLEAN=1; shift ;;
    --no-compdb) DO_COMPDB=0; shift ;;
    --compdb-only) DO_COMPDB=1; CMD=compdb; shift ;;
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

# Board QEMU profile (from boards/*/board.mk via make qemu-cmd)
QEMU_LOAD="bios_bin"
QEMU_MACHINE=()

load_board_qemu() {
  local line qflags
  if [[ ! -f "boards/${BOARD}/board.mk" ]]; then
    echo "ERROR: unknown board '${BOARD}' (boards/${BOARD}/board.mk missing)"
    exit 2
  fi
  while IFS= read -r line; do
    case "${line}" in
      QEMU=*) QEMU="${line#QEMU=}" ;;
      QEMU_LOAD=*) QEMU_LOAD="${line#QEMU_LOAD=}" ;;
      QEMU_ARCH=*) BOARD_ARCH="${line#QEMU_ARCH=}" ;;
      QEMU_CROSS=*) CROSS_PREFIX="${line#QEMU_CROSS=}" ;;
      QEMU_FLAGS=*)
        qflags="${line#QEMU_FLAGS=}"
        # shellcheck disable=SC2206
        QEMU_MACHINE=( ${qflags} )
        ;;
    esac
  done < <(make -s qemu-cmd BOARD="${BOARD}" CORES="${CORES}" CPU="${CPU}" 2>/dev/null)
  if [[ -n "${QEMU_OVERRIDE}" ]]; then
    QEMU="${QEMU_OVERRIDE}"
  fi
  if [[ -z "${QEMU}" || ${#QEMU_MACHINE[@]} -eq 0 ]]; then
    echo "ERROR: make qemu-cmd failed for BOARD=${BOARD}"
    make qemu-cmd BOARD="${BOARD}" CORES="${CORES}" CPU="${CPU}" || true
    exit 1
  fi
}

# Setup PATH / GDB for current BOARD_ARCH after load_board_qemu.
setup_arch_env() {
  if [[ "${BOARD_ARCH}" == "arm64" ]]; then
    if [[ -d "${TOOLS_ROOT}/usr/bin" ]]; then
      export PATH="${TOOLS_ROOT}/usr/bin:${PATH}"
      if [[ -d "${TOOLS_ROOT}/usr/lib/x86_64-linux-gnu" ]]; then
        export LD_LIBRARY_PATH="${TOOLS_ROOT}/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
      fi
    fi
    CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
    if [[ -z "${GDB}" ]]; then
      GDB="$(command -v aarch64-linux-gnu-gdb || true)"
      [[ -n "${GDB}" ]] || GDB="$(command -v gdb-multiarch || true)"
    fi
  else
    export PATH="${SYSROOT}/bin:${PATH}"
    CROSS_PREFIX="${CROSS_PREFIX:-riscv64-unknown-linux-gnu-}"
    if [[ -z "${GDB}" ]]; then
      if [[ -x "${SYSROOT}/bin/riscv64-unknown-linux-gnu-gdb" ]]; then
        GDB="${SYSROOT}/bin/riscv64-unknown-linux-gnu-gdb"
      else
        GDB="$(command -v riscv64-unknown-linux-gnu-gdb || true)"
      fi
    fi
  fi
}

load_board_qemu
setup_arch_env

qemu_image_args() {
  if [[ "${QEMU_LOAD}" == "elf" ]]; then
    echo -bios none -kernel cgrtos.elf
  elif [[ "${QEMU_LOAD}" == "kernel" ]]; then
    echo -kernel cgrtos.elf
  else
    echo -bios cgrtos.bin
  fi
}

need_toolchain() {
  local gcc_bin
  gcc_bin="$(command -v "${CROSS_PREFIX}gcc" || true)"
  if [[ -z "${gcc_bin}" ]]; then
    echo "ERROR: ${CROSS_PREFIX}gcc not found (BOARD=${BOARD} ARCH=${BOARD_ARCH})"
    if [[ "${BOARD_ARCH}" == "arm64" ]]; then
      echo "  Hint: export PATH=${TOOLS_ROOT}/usr/bin:\$PATH"
    else
      echo "  Hint: SYSROOT=${SYSROOT}"
    fi
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
  if [[ -z "${GDB}" || ! -x "${GDB}" ]]; then
    echo "ERROR: GDB not found for ARCH=${BOARD_ARCH} (set GDB=...)"
    exit 1
  fi
}

make_vars() {
  echo "APP=${APP}" "CORES=${CORES}" "BOARD=${BOARD}" "PROFILE=${PROFILE}"
}

# ---------------------------------------------------------------------------
# clangd: generate compile_commands.json via bear (preferred) or make -n
# ---------------------------------------------------------------------------
do_compdb() {
  need_toolchain
  local cc_path
  cc_path="$(command -v "${CROSS_PREFIX}gcc")"
  echo "==> Generating compile_commands.json (APP=${APP} CORES=${CORES} BOARD=${BOARD} ARCH=${BOARD_ARCH})"
  if command -v bear >/dev/null 2>&1; then
    make clean APP="${APP}" CORES="${CORES}" BOARD="${BOARD}" CPU="${CPU}" PROFILE="${PROFILE}" >/dev/null
    bear --output compile_commands.json -- \
      make all APP="${APP}" CORES="${CORES}" BOARD="${BOARD}" CPU="${CPU}" PROFILE="${PROFILE}"
  else
    echo "WARN: bear not found — writing a minimal compile_commands.json via make -n"
    python3 - "${APP}" "${CORES}" "${BOARD}" "${PROFILE}" "${cc_path}" "${ROOT}" <<'PY'
import json, os, subprocess, sys
app, cores, board, profile, cc, root = sys.argv[1:7]
env = os.environ.copy()
cmd = ["make", "-n", "all", f"APP={app}", f"CORES={cores}", f"BOARD={board}", f"PROFILE={profile}"]
out = subprocess.check_output(cmd, cwd=root, env=env, text=True, stderr=subprocess.STDOUT)
entries = []
for line in out.splitlines():
    line = line.strip()
    if " -c " not in line:
        continue
    parts = line.split()
    src = None
    for p in reversed(parts):
        if p.endswith(".c") or p.endswith(".S"):
            src = p
            break
    if not src:
        continue
    cmd_line = line
    if not any(x.endswith("gcc") or "gcc" in x for x in parts[:1]):
        cmd_line = cc + " " + line
    elif parts[0].endswith("gcc") and not os.path.isabs(parts[0]):
        cmd_line = cc + " " + " ".join(parts[1:])
    entries.append({
        "directory": root,
        "command": cmd_line,
        "file": os.path.join(root, src) if not os.path.isabs(src) else src,
    })
with open(os.path.join(root, "compile_commands.json"), "w") as f:
    json.dump(entries, f, indent=2)
    f.write("\n")
print(f"wrote {len(entries)} entries using {cc}")
PY
  fi
  if [[ ! -f .clangd ]]; then
    cat > .clangd <<'EOF'
CompileFlags:
  CompilationDatabase: .
Diagnostics:
  UnusedIncludes: None
Index:
  Background: Build
EOF
  fi
  echo "==> clangd: open the workspace root; compile_commands.json ready"
}

do_build() {
  need_toolchain
  echo "==> Building APP=${APP} CORES=${CORES} BOARD=${BOARD} ARCH=${BOARD_ARCH} CPU=${CPU:-default} PROFILE=${PROFILE:-default}"
  # APP/CORES/CPU/PROFILE stamp；对象已按 BOARD 分目录，仍用 stamp 避免错链旧 elf
  local stamp=".build_stamp"
  local want="${BOARD}|${CPU}|${CORES}|${APP}|${PROFILE}"
  local have=""
  [[ -f "${stamp}" ]] && have="$(cat "${stamp}")"
  if [[ "${DO_CLEAN}" -eq 1 || "${have}" != "${want}" ]]; then
    echo "==> clean (board/config stamp changed)"
    make clean APP="${APP}" CORES="${CORES}" BOARD="${BOARD}" CPU="${CPU}" PROFILE="${PROFILE}"
  fi
  if [[ "${DO_COMPDB}" -eq 1 ]] && command -v bear >/dev/null 2>&1; then
    echo "==> clangd: refreshing compile_commands.json via bear"
    bear --output compile_commands.json -- \
      make -B all APP="${APP}" CORES="${CORES}" BOARD="${BOARD}" CPU="${CPU}" PROFILE="${PROFILE}"
  else
    make all APP="${APP}" CORES="${CORES}" BOARD="${BOARD}" CPU="${CPU}" PROFILE="${PROFILE}"
    if [[ "${DO_COMPDB}" -eq 1 ]]; then
      do_compdb || true
    fi
  fi
  echo "${want}" >"${stamp}"
  echo "==> Built: cgrtos.elf / cgrtos.bin"
  ls -la cgrtos.elf cgrtos.bin 2>/dev/null || true
}

require_binary() {
  if [[ "${QEMU_LOAD}" == "elf" || "${QEMU_LOAD}" == "kernel" ]]; then
    if [[ ! -f cgrtos.elf ]]; then
      echo "ERROR: cgrtos.elf missing — compile first:"
      echo "  ./scripts/cgrtos.sh build --app ${APP} --cores ${CORES} --board ${BOARD}"
      exit 1
    fi
  elif [[ ! -f cgrtos.bin ]]; then
    echo "ERROR: cgrtos.bin missing — compile first:"
    echo "  ./scripts/cgrtos.sh build --app ${APP} --cores ${CORES} --board ${BOARD}"
    exit 1
  fi
  if [[ "${WITH_GDB}" -eq 1 && ! -f cgrtos.elf ]]; then
    echo "ERROR: cgrtos.elf missing (needed for GDB)"
    exit 1
  fi
}

# Optional: build if missing / unless --no-build for convenience cmds
ensure_built() {
  if [[ "${NO_BUILD}" -eq 1 ]]; then
    require_binary
    return
  fi
  do_build
}

# ---------------------------------------------------------------------------
# Open GDB (+ TUI C source) in a second window / pane.
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
sleep 1
exec $(printf '%q' "${GDB}") -q -tui \\
  -ex $(printf '%q' "target remote :${GDB_PORT}") \\
  -x $(printf '%q' "${ROOT}/scripts/gdbinit") \\
  $(printf '%q' "${ROOT}/cgrtos.elf")
EOF
  chmod +x "${gdb_script}"

  local prefer="${CGRTOS_GDB_TERM:-auto}"
  local launched=0

  try_wt() {
    local wt=""
    local candidates=(
      "${CGRTOS_WT:-}"
      "$(command -v wt.exe 2>/dev/null || true)"
    )
    shopt -s nullglob
    candidates+=(
      /mnt/c/Users/*/AppData/Local/Microsoft/WindowsApps/Microsoft.WindowsTerminal_*/wt.exe
      /mnt/c/Users/*/AppData/Local/Microsoft/WindowsApps/wt.exe
    )
    shopt -u nullglob
    local c
    for c in "${candidates[@]}"; do
      [[ -n "${c}" && -x "${c}" ]] || continue
      wt="${c}"
      break
    done
    [[ -n "${wt}" ]] || return 1
    "${wt}" new-tab --title "CG-RTOS GDB" bash "${gdb_script}" && launched=1
  }

  try_tmux() {
    command -v tmux >/dev/null 2>&1 || return 1
    [[ -n "${TMUX:-}" ]] || return 1
    tmux split-window -h "bash ${gdb_script}" && launched=1
  }

  try_xterm() {
    if command -v x-terminal-emulator >/dev/null 2>&1; then
      x-terminal-emulator -e bash "${gdb_script}" &
      launched=1
      return 0
    fi
    if command -v gnome-terminal >/dev/null 2>&1; then
      gnome-terminal -- bash "${gdb_script}" &
      launched=1
      return 0
    fi
    return 1
  }

  case "${prefer}" in
    wt) try_wt || true ;;
    tmux) try_tmux || true ;;
    xterm) try_xterm || true ;;
    here)
      echo "==> GDB in this terminal after QEMU starts (Ctrl-C QEMU first if stuck)"
      bash "${gdb_script}" &
      launched=1
      ;;
    auto)
      try_wt || try_tmux || try_xterm || {
        echo "==> Could not open second window; GDB script: ${gdb_script}"
        echo "    Run manually: bash ${gdb_script}"
      }
      ;;
  esac
  if [[ "${launched}" -eq 1 ]]; then
    echo "==> GDB window launched"
  fi
}

do_run_qemu() {
  need_qemu
  require_binary
  local LOG
  local -a IMG_ARGS
  LOG="$(mktemp /tmp/cgrtos-qemu.XXXXXX.log)"
  # shellcheck disable=SC2064
  trap "rm -f '${LOG}'" EXIT
  # shellcheck disable=SC2207
  IMG_ARGS=( $(qemu_image_args) )
  echo "==> QEMU BOARD=${BOARD} APP=${APP} CORES=${CORES} timeout=${TIMEOUT_SEC}s load=${QEMU_LOAD}"
  if [[ "${WITH_GDB}" -eq 1 ]]; then
    open_gdb_window
    echo "==> QEMU + gdbstub :${GDB_PORT} (UART here; GDB elsewhere)"
    "${QEMU}" "${QEMU_MACHINE[@]}" "${IMG_ARGS[@]}" -S -gdb "tcp::${GDB_PORT}" \
      2>&1 | tee "${LOG}"
    return "${PIPESTATUS[0]}"
  fi
  set +e
  timeout "${TIMEOUT_SEC}" "${QEMU}" "${QEMU_MACHINE[@]}" "${IMG_ARGS[@]}" \
    >"${LOG}" 2>&1
  local QEMU_RC=$?
  set -e

  local OUT
  OUT="$(tr -d '\r' <"${LOG}")"
  printf '%s\n' "${OUT}"
  echo ""
  echo "==> Summary"
  local PASS_N FAIL_N BENCH_N
  PASS_N="$(printf '%s\n' "${OUT}" | grep -c '\[PASS\]' || true)"
  FAIL_N="$(printf '%s\n' "${OUT}" | grep -c '\[FAIL\]' || true)"
  BENCH_N="$(printf '%s\n' "${OUT}" | grep -c '^BENCH ' || true)"
  echo "  [PASS]=${PASS_N}  [FAIL]=${FAIL_N}  BENCH=${BENCH_N}"

  case "${APP}" in
    bench)
      if printf '%s\n' "${OUT}" | grep -q '=== BENCH_DONE ==='; then
        echo "---- Bench ----"
        printf '%s\n' "${OUT}" | grep '^BENCH ' || true
        echo "RESULT: BENCH_DONE"; return 0
      fi
      echo "RESULT: BENCH_INCOMPLETE"; return 1
      ;;
    stress)
      if printf '%s\n' "${OUT}" | grep -q '=== STRESS_PASSED ==='; then
        echo "RESULT: STRESS_PASSED"; return 0
      fi
      if printf '%s\n' "${OUT}" | grep -q '=== STRESS_FAILED ==='; then
        echo "RESULT: STRESS_FAILED"; return 1
      fi
      if [[ "${QEMU_RC}" -eq 124 ]]; then
        echo "RESULT: TIMEOUT"; return 1
      fi
      echo "RESULT: STRESS_INCOMPLETE (qemu rc=${QEMU_RC})"; return 1
      ;;
    demo)
      if printf '%s\n' "${OUT}" | grep -qE '\[DEMO\] (produce|consume|heartbeat)|HAL board:|\[T1\] LED OFF'; then
        echo "RESULT: DEMO_OK"; return 0
      fi
      echo "RESULT: DEMO_INCOMPLETE"; return 1
      ;;
    test|*)
      if printf '%s\n' "${OUT}" | grep -q '=== TEST_SUITE_PASSED ==='; then
        echo "RESULT: TEST_SUITE_PASSED"; return 0
      fi
      if printf '%s\n' "${OUT}" | grep -qE 'Results: [0-9]+ passed, 0 failed'; then
        echo "RESULT: TEST_SUITE_PASSED"; return 0
      fi
      if printf '%s\n' "${OUT}" | grep -q '=== TEST_SUITE_FAILED ==='; then
        echo "RESULT: TEST_SUITE_FAILED"; return 1
      fi
      if [[ "${QEMU_RC}" -eq 124 ]]; then
        echo "RESULT: TIMEOUT"; return 1
      fi
      echo "RESULT: NO_SUITE_MARKER (qemu rc=${QEMU_RC})"; return 1
      ;;
  esac
}

do_run() {
  # Pure run: never builds. Convenience apps call ensure_built before this.
  do_run_qemu
}

do_cli() {
  APP=cli
  ensure_built
  need_qemu
  require_binary
  local -a IMG_ARGS
  # shellcheck disable=SC2207
  IMG_ARGS=( $(qemu_image_args) )
  echo "==> CLI BOARD=${BOARD} (interactive). Exit QEMU: Ctrl-A then X"
  if [[ "${WITH_GDB}" -eq 1 ]]; then
    open_gdb_window
    exec "${QEMU}" "${QEMU_MACHINE[@]}" "${IMG_ARGS[@]}" -S -gdb "tcp::${GDB_PORT}"
  fi
  exec "${QEMU}" "${QEMU_MACHINE[@]}" "${IMG_ARGS[@]}"
}

do_gdb() {
  WITH_GDB=1
  ensure_built
  do_run_qemu
}

do_docs() {
  if ! command -v doxygen >/dev/null 2>&1; then
    echo "ERROR: doxygen not installed"
    exit 1
  fi
  doxygen Doxyfile
  echo "==> docs/doxygen/html/index.html"
}

do_sdk() {
  do_docs
  local sdk="${ROOT}/sdk"
  rm -rf "${sdk}"
  mkdir -p "${sdk}/include/hal" "${sdk}/docs"
  cp -a kernel/cgrtos.h kernel/cgrtos_config.h kernel/list.h kernel/vfs.h "${sdk}/include/" 2>/dev/null || \
    cp -a kernel/cgrtos.h kernel/cgrtos_config.h kernel/list.h "${sdk}/include/"
  cp -a hal/hal.h hal/hal_board.h hal/hal_drv.h "${sdk}/include/hal/" 2>/dev/null || true
  if [[ -d docs/doxygen/html ]]; then
    cp -a docs/doxygen/html "${sdk}/docs/api"
  fi
  cat >"${sdk}/README.md" <<'EOF'
# CG-RTOS SDK

| Path | Content |
|------|---------|
| `include/cgrtos.h` | Public kernel API |
| `include/cgrtos_config.h` | Feature / capacity knobs |
| `include/hal/` | HAL headers |
| `docs/api/index.html` | Doxygen |

See repository `docs/USER_GUIDE.md` and `docs/SCRIPTS.md`.
EOF
  echo "==> SDK ready under sdk/"
}

print_help() {
  cat <<'EOF'
CG-RTOS — multi-arch build and run (build ≠ run)

Usage:
  ./scripts/cgrtos.sh <command> [options]

── Compile only ──────────────────────────────────────────
  build       Compile APP → cgrtos.elf / cgrtos.bin
              Objects under build/<BOARD>/; also refresh clangd DB unless --no-compdb
  compdb      Only regenerate compile_commands.json for clangd

── Run only (requires prior build) ───────────────────────
  run         Boot existing image in QEMU (never runs make)

── Convenience (build then run) ──────────────────────────
  test        build --app test  && run   (timeout default 120s)
  demo        build --app demo  && run
  bench       build --app bench && run   (timeout default 30s)
  stress      build --app stress&& run   (timeout default 60s)
  cli         build --app cli   && interactive QEMU
  gdb         build + QEMU(-S) + GDB TUI

── Docs / port check ─────────────────────────────────────
  docs        Doxygen → docs/doxygen/html/
  sdk         docs + package sdk/
  help        This message

Options:
  --app NAME       demo | test | bench | stress | cli
  --cores N        1 | 2 | 4 (default 2; ARM64 virt max 1)
  --board NAME     nuclei_evalsoc | riscv_virt | sifive_u | qemu_virt_a64
  --cpu NAME       e.g. nuclei-nx900fd | rv64 | sifive-u54 | cortex-a53 | cortex-a72
  --profile NAME   e.g. minimal → -DCGRTOS_CONFIG_MINIMAL=1
  --clean          make clean before build
  --no-compdb      skip compile_commands.json on build
  --no-build       skip make for convenience cmds (use existing binary)
  --gdb | -g       QEMU gdbstub + second-window GDB TUI
  --timeout SEC    QEMU wall timeout (default 120)
  --port N         GDB port (default 1234)

Examples:
  ./scripts/cgrtos.sh test --board riscv_virt --cores 2 --clean
  ./scripts/cgrtos.sh stress --board riscv_virt --cores 2
  ./scripts/cgrtos.sh demo --board nuclei_evalsoc --cpu nuclei-nx900fd --cores 1
  ./scripts/cgrtos.sh demo --board sifive_u --cores 2
  # ARM64 (A55/A75 class stand-ins on QEMU 6.2)
  export PATH=/home/cong/cgrtos-tools/root/usr/bin:$PATH
  export LD_LIBRARY_PATH=/home/cong/cgrtos-tools/root/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
  ./scripts/cgrtos.sh test --board qemu_virt_a64 --cores 1 --cpu cortex-a53 --clean
  ./scripts/cgrtos.sh demo --board qemu_virt_a64 --cores 1 --cpu cortex-a72

  # One-shot port gate:
  ./scripts/port-check.sh

Environment: SYSROOT, TOOLS_ROOT, QEMU, GDB, CORES, BOARD, PROFILE, CGRTOS_GDB_TERM

Full guide: docs/SCRIPTS.md | Porting: docs/PORTING.md
EOF
}

case "${CMD}" in
  help|-h|--help) print_help ;;
  build) do_build ;;
  compdb) do_compdb ;;
  run)
    # Pure run — no make
    do_run
    ;;
  test) APP=test; ensure_built; do_run ;;
  demo) APP=demo; ensure_built; do_run ;;
  bench)
    APP=bench
    # Only override default 120 if user did not pass --timeout
    if [[ "${TIMEOUT_SEC}" == "120" ]]; then TIMEOUT_SEC=30; fi
    ensure_built
    do_run
    ;;
  stress)
    APP=stress
    if [[ "${TIMEOUT_SEC}" == "120" ]]; then TIMEOUT_SEC=60; fi
    ensure_built
    do_run
    ;;
  cli) do_cli ;;
  gdb) do_gdb ;;
  docs) do_docs ;;
  sdk) do_sdk ;;
  *) echo "Unknown command: ${CMD}"; print_help; exit 2 ;;
esac
