#!/usr/bin/env bash
# =============================================================================
# CG-RTOS — 编译 / 运行 / clangd / 文档 统一入口
# =============================================================================
# 设计原则：编译与运行分离。
#   build   → 只编译（可选生成 compile_commands.json）
#   run     → 只运行已有 cgrtos.bin（不自动 make）
#   test/…  → 先 build 再 run（可用 --no-build）
#
# 详细说明见：docs/SCRIPTS.md
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

APP="${APP:-test}"
NO_BUILD=0
DO_CLEAN=0
DO_COMPDB=1
TIMEOUT_SEC="${TIMEOUT_SEC:-120}"
GDB_PORT=1234
WITH_GDB=0
CORES="${CORES:-2}"
BOARD="${BOARD:-nuclei_evalsoc}"
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

QEMU_MACHINE=(
  -M nuclei_evalsoc,download=ddr
  -smp "${CORES}" -m 512M
  -cpu nuclei-ux900fd,ext=svpbmt_zicbom_sstc_sscofpmf_zba_zbb_zbc_zbs_zicond
  -nographic -serial mon:stdio
)
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

make_vars() {
  echo "APP=${APP}" "CORES=${CORES}" "BOARD=${BOARD}" "PROFILE=${PROFILE}"
}

# ---------------------------------------------------------------------------
# clangd: generate compile_commands.json via bear (preferred) or make -n
# ---------------------------------------------------------------------------
do_compdb() {
  need_toolchain
  echo "==> Generating compile_commands.json (APP=${APP} CORES=${CORES} BOARD=${BOARD})"
  # Touch a stamp so make rebuilds enough entries; bear wraps the real compile.
  if command -v bear >/dev/null 2>&1; then
    # Rebuild objects under bear to refresh the DB (no link required for clangd)
    make clean APP="${APP}" CORES="${CORES}" BOARD="${BOARD}" PROFILE="${PROFILE}" >/dev/null
    bear --output compile_commands.json -- \
      make all APP="${APP}" CORES="${CORES}" BOARD="${BOARD}" PROFILE="${PROFILE}"
  else
    echo "WARN: bear not found — writing a minimal compile_commands.json via make -n"
    python3 - "${APP}" "${CORES}" "${BOARD}" "${PROFILE}" "${SYSROOT}" "${ROOT}" <<'PY'
import json, os, subprocess, sys
app, cores, board, profile, sysroot, root = sys.argv[1:7]
env = os.environ.copy()
env["PATH"] = f"{sysroot}/bin:" + env.get("PATH", "")
cmd = ["make", "-n", "all", f"APP={app}", f"CORES={cores}", f"BOARD={board}", f"PROFILE={profile}"]
out = subprocess.check_output(cmd, cwd=root, env=env, text=True, stderr=subprocess.STDOUT)
entries = []
cc = f"{sysroot}/bin/riscv64-unknown-linux-gnu-gcc"
for line in out.splitlines():
    line = line.strip()
    if " -c " not in line or ".c" not in line:
        continue
    # last token ending with .c is the source
    parts = line.split()
    src = None
    for p in reversed(parts):
        if p.endswith(".c") or p.endswith(".S"):
            src = p
            break
    if not src:
        continue
    entries.append({
        "directory": root,
        "command": line if line.startswith("/") or "gcc" in line else f"{cc} " + " ".join(parts[1:]) if parts[0].endswith("gcc") else line,
        "file": os.path.join(root, src) if not os.path.isabs(src) else src,
    })
# Fix relative gcc in command
for e in entries:
    c = e["command"]
    if c.startswith("riscv64-") or c.startswith("$(CROSS)"):
        e["command"] = cc + " " + " ".join(c.split()[1:])
    elif not c.startswith("/"):
        # "gcc ..." from make uses $(CC) expanded
        pass
with open(os.path.join(root, "compile_commands.json"), "w") as f:
    json.dump(entries, f, indent=2)
    f.write("\n")
print(f"wrote {len(entries)} entries")
PY
  fi
  # Ensure .clangd points at the DB
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
  echo "    (Cursor/VS Code: reload window if index is stale)"
}

do_build() {
  need_toolchain
  echo "==> Building APP=${APP} CORES=${CORES} BOARD=${BOARD} PROFILE=${PROFILE:-default}"
  if [[ "${DO_CLEAN}" -eq 1 ]]; then
    make clean APP="${APP}" CORES="${CORES}" BOARD="${BOARD}" PROFILE="${PROFILE}"
  fi
  if [[ "${DO_COMPDB}" -eq 1 ]] && command -v bear >/dev/null 2>&1; then
    # Force-rebuild under bear so compile_commands.json covers every TU (clangd)
    echo "==> clangd: refreshing compile_commands.json via bear"
    bear --output compile_commands.json -- \
      make -B all APP="${APP}" CORES="${CORES}" BOARD="${BOARD}" PROFILE="${PROFILE}"
  else
    make all APP="${APP}" CORES="${CORES}" BOARD="${BOARD}" PROFILE="${PROFILE}"
    if [[ "${DO_COMPDB}" -eq 1 ]]; then
      do_compdb || true
    fi
  fi
  echo "==> Built: cgrtos.elf / cgrtos.bin"
  ls -la cgrtos.elf cgrtos.bin 2>/dev/null || true
}

require_binary() {
  if [[ ! -f cgrtos.bin ]]; then
    echo "ERROR: cgrtos.bin missing — compile first:"
    echo "  ./scripts/cgrtos.sh build --app ${APP} --cores ${CORES}"
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
  LOG="$(mktemp /tmp/cgrtos-qemu.XXXXXX.log)"
  # shellcheck disable=SC2064
  trap "rm -f '${LOG}'" EXIT
  echo "==> QEMU APP=${APP} CORES=${CORES} timeout=${TIMEOUT_SEC}s (no rebuild)"
  if [[ "${WITH_GDB}" -eq 1 ]]; then
    open_gdb_window
    echo "==> QEMU + gdbstub :${GDB_PORT} (UART here; GDB elsewhere)"
    "${QEMU}" "${QEMU_MACHINE[@]}" -bios cgrtos.bin -S -gdb "tcp::${GDB_PORT}" \
      2>&1 | tee "${LOG}"
    return "${PIPESTATUS[0]}"
  fi
  set +e
  timeout "${TIMEOUT_SEC}" "${QEMU}" "${QEMU_MACHINE[@]}" -bios cgrtos.bin \
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
      if printf '%s\n' "${OUT}" | grep -q '\[T1\] LED OFF'; then
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
  echo "==> CLI (interactive). Exit QEMU: Ctrl-A then X"
  if [[ "${WITH_GDB}" -eq 1 ]]; then
    open_gdb_window
    exec "${QEMU}" "${QEMU_MACHINE[@]}" -bios cgrtos.bin -S -gdb "tcp::${GDB_PORT}"
  fi
  exec "${QEMU}" "${QEMU_MACHINE[@]}" -bios cgrtos.bin
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
CG-RTOS — build and run are separate

Usage:
  ./scripts/cgrtos.sh <command> [options]

── Compile only ──────────────────────────────────────────
  build       Compile APP → cgrtos.elf / cgrtos.bin
              Also refreshes compile_commands.json (clangd) unless --no-compdb
  compdb      Only regenerate compile_commands.json for clangd

── Run only (requires prior build) ───────────────────────
  run         Boot existing cgrtos.bin in QEMU (never runs make)

── Convenience (build then run) ──────────────────────────
  test        build --app test  && run
  demo        build --app demo  && run
  bench       build --app bench && run   (timeout default 30s)
  stress      build --app stress&& run   (timeout default 60s)
  cli         build --app cli   && interactive QEMU
  gdb         build + QEMU(-S) + GDB TUI

── Docs ──────────────────────────────────────────────────
  docs        Doxygen → docs/doxygen/html/
  sdk         docs + package sdk/
  help        This message

Options:
  --app NAME       demo | test | bench | stress | cli
  --cores N        1 | 2 | 4 (default 2)
  --board NAME     BSP under boards/ (default nuclei_evalsoc)
  --profile NAME   e.g. minimal → -DCGRTOS_CONFIG_MINIMAL=1
  --clean          make clean before build
  --no-compdb      skip compile_commands.json on build
  --no-build       skip make for convenience cmds (use existing binary)
  --gdb | -g       QEMU gdbstub + second-window GDB TUI
  --timeout SEC    QEMU wall timeout (default 120)
  --port N         GDB port (default 1234)

Examples:
  # 1) Compile for clangd + tests
  ./scripts/cgrtos.sh build --app test --cores 2 --clean

  # 2) Run without rebuilding
  ./scripts/cgrtos.sh run --app test --cores 2 --timeout 180

  # 3) One-shot suite
  ./scripts/cgrtos.sh test --cores 2

  # 4) Minimal trim compile
  ./scripts/cgrtos.sh build --app demo --cores 1 --profile minimal

  # 5) Refresh clangd DB only
  ./scripts/cgrtos.sh compdb --app test --cores 2

Environment: SYSROOT, QEMU, GDB, CORES, BOARD, PROFILE, CGRTOS_GDB_TERM

Full guide: docs/SCRIPTS.md
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
