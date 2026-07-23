#!/usr/bin/env bash
# =============================================================================
# CG-RTOS port-check — 一键验证多架构 build + test + stress + demo smoke
# =============================================================================
# 默认门禁：
#   1) riscv_virt  CORES=2  test + stress
#   2) qemu_virt_a64 CORES=1 test + stress（需 aarch64 工具链）
# 可选 smoke（设 CGRTOS_PORT_SMOKE=1）：
#   nuclei_evalsoc / sifive_u / a72 demo
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SH="${ROOT}/scripts/cgrtos.sh"
FAIL=0

run_one() {
  local title="$1"
  shift
  echo ""
  echo "======== ${title} ========"
  if "$@"; then
    echo "OK: ${title}"
  else
    echo "FAIL: ${title}"
    FAIL=1
  fi
}

run_one "riscv_virt test" \
  "${SH}" test --board riscv_virt --cores 2 --cpu rv64 --clean --no-compdb --timeout 180

run_one "riscv_virt stress" \
  "${SH}" stress --board riscv_virt --cores 2 --cpu rv64 --no-compdb --timeout 120

# ARM64 tools
if [[ -d /home/cong/cgrtos-tools/root/usr/bin ]]; then
  export PATH="/home/cong/cgrtos-tools/root/usr/bin:${PATH}"
  export LD_LIBRARY_PATH="/home/cong/cgrtos-tools/root/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
fi

if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  run_one "qemu_virt_a64 test (cortex-a53)" \
    "${SH}" test --board qemu_virt_a64 --cores 1 --cpu cortex-a53 --clean --no-compdb --timeout 180

  run_one "qemu_virt_a64 stress (cortex-a53)" \
    "${SH}" stress --board qemu_virt_a64 --cores 1 --cpu cortex-a53 --no-compdb --timeout 120
else
  echo "SKIP: ARM64 toolchain missing"
fi

if [[ "${CGRTOS_PORT_SMOKE:-0}" == "1" ]]; then
  run_one "nuclei_evalsoc demo nx900" \
    "${SH}" demo --board nuclei_evalsoc --cpu nuclei-nx900fd --cores 1 --clean --no-compdb --timeout 8
  run_one "sifive_u demo" \
    "${SH}" demo --board sifive_u --cores 2 --clean --no-compdb --timeout 8
  if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    run_one "qemu_virt_a64 demo a72" \
      "${SH}" demo --board qemu_virt_a64 --cores 1 --cpu cortex-a72 --no-compdb --timeout 5
  fi
fi

echo ""
if [[ "${FAIL}" -eq 0 ]]; then
  echo "======== PORT CHECK PASSED ========"
  exit 0
fi
echo "======== PORT CHECK FAILED ========"
exit 1
