# CG-RTOS — multi-board / multi-arch (RISC-V + AArch64)
#
# Usage:
#   make BOARD=nuclei_evalsoc CPU=nuclei-ux900fd APP=demo
#   make BOARD=riscv_virt     CPU=rv64            APP=demo
#   make BOARD=sifive_u       CPU=sifive-u54      APP=demo CORES=2
#   make BOARD=qemu_virt_a64  CPU=cortex-a53      APP=demo CORES=1
# Prefer: ./scripts/cgrtos.sh <cmd> --board …
#
# Objects live under build/$(BOARD)/ so switching boards does not mix .o files.
# Firmware artifacts cgrtos.elf / cgrtos.bin stay at repo root for scripts.

APP     ?= demo
CORES   ?= 2
BOARD   ?= nuclei_evalsoc
CPU     ?=
PROFILE ?=
ifeq ($(filter $(CORES),1 2 4),)
  $(error CORES must be 1, 2, or 4 (got '$(CORES)'))
endif

# ---- Board BSP -------------------------------------------------------------
BOARD_MK := boards/$(BOARD)/board.mk
ifeq ($(wildcard $(BOARD_MK)),)
  $(error Unknown BOARD='$(BOARD)' (missing $(BOARD_MK)))
endif
include $(BOARD_MK)

ifneq ($(CPU),)
  BOARD_QEMU_CPU := $(CPU)
endif

ifdef BOARD_QEMU_MAX_CORES
  ifeq ($(shell test $(CORES) -gt $(BOARD_QEMU_MAX_CORES) && echo 1),1)
    $(error BOARD=$(BOARD) supports at most CORES=$(BOARD_QEMU_MAX_CORES))
  endif
endif
ifdef BOARD_QEMU_MIN_CORES
  ifeq ($(shell test $(CORES) -lt $(BOARD_QEMU_MIN_CORES) && echo 1),1)
    $(error BOARD=$(BOARD) requires at least CORES=$(BOARD_QEMU_MIN_CORES))
  endif
endif

BOARD_ARCH ?= riscv

# Toolchain (board may override CROSS / SYSROOT / QEMU)
ifeq ($(BOARD_ARCH),arm64)
  CROSS   ?= aarch64-linux-gnu-
  SYSROOT ?=
  TOOLS_ROOT ?= /home/cong/cgrtos-tools/root
  ifneq ($(wildcard $(TOOLS_ROOT)/usr/bin/$(CROSS)gcc),)
    export PATH := $(TOOLS_ROOT)/usr/bin:$(PATH)
    export LD_LIBRARY_PATH := $(TOOLS_ROOT)/usr/lib/x86_64-linux-gnu:$(LD_LIBRARY_PATH)
  endif
else
  CROSS   ?= riscv64-unknown-linux-gnu-
  SYSROOT ?= /home/cong/nuclei-ux900fd-linux/nuclei-linux-sdk/work/evalsoc/buildroot_initramfs/host/opt/ext-toolchain
  export PATH := $(SYSROOT)/bin:$(PATH)
endif
ifneq ($(BOARD_CROSS),)
  CROSS := $(BOARD_CROSS)
endif
ifneq ($(BOARD_SYSROOT),)
  SYSROOT := $(BOARD_SYSROOT)
endif

CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
QEMU    ?= $(BOARD_QEMU)

ifeq ($(APP),test)
  APP_SRCS := tests/test_all.c tests/test_cases.c tests/stress_cases.c
else ifeq ($(APP),cli)
  APP_SRCS := cli/cli_main.c cli/cli_fs.c cli/cli_path.c \
              cli/cli_line.c cli/cli_vim.c \
              tests/test_cases.c tests/stress_cases.c
else ifeq ($(APP),bench)
  APP_SRCS := tests/bench_all.c
else ifeq ($(APP),stress)
  APP_SRCS := tests/stress_all.c tests/stress_cases.c
else
  APP_SRCS := $(wildcard demo/*.c)
endif

# boards/$(BOARD) MUST precede -Ihal so #include "hal_board.h" hits BSP
COMMON_CFLAGS := -nostdlib -nostartfiles -ffreestanding -fno-builtin \
           -fno-stack-protector -O2 -g -Wall \
           -DCONFIG_NUM_CORES=$(CORES) \
           -DCONFIG_TIMER_CLOCK_HZ=$(BOARD_TIMER_HZ)ULL \
           $(BOARD_CFLAGS) \
           -I. -Iboards/$(BOARD) -Ikernel -Ihal

ifeq ($(BOARD_ARCH),arm64)
  CFLAGS := -march=armv8-a -mgeneral-regs-only -mno-outline-atomics \
            -fno-pic -fno-pie $(COMMON_CFLAGS)
  LDFLAGS := -nostdlib -nostartfiles -ffreestanding -no-pie -T $(BOARD_LINK_SCRIPT)
else
  CFLAGS := -march=rv64imafdc -mabi=lp64d -mcmodel=medany \
            $(COMMON_CFLAGS) \
            -I$(SYSROOT)/riscv64-unknown-linux-gnu/sysroot/usr/include
  LDFLAGS := -nostdlib -nostartfiles -ffreestanding -T $(BOARD_LINK_SCRIPT)
endif
ifeq ($(PROFILE),minimal)
  CFLAGS += -DCGRTOS_CONFIG_MINIMAL=1
endif

LINK_SCRIPT := $(BOARD_LINK_SCRIPT)
ifeq ($(BOARD_ARCH),arm64)
  LIBGCC :=
else
  LIBGCC := -lgcc
endif

ARCH_DIR := arch/$(BOARD_ARCH)
ARCH_ALL := $(wildcard $(ARCH_DIR)/*.c)
ifeq ($(BOARD_ARCH),arm64)
  ARCH_SRCS := $(ARCH_ALL)
else
  ARCH_SRCS := $(filter-out $(ARCH_DIR)/uart.c $(ARCH_DIR)/uart_ns16550.c,$(ARCH_ALL)) \
               $(ARCH_DIR)/$(BOARD_UART_DRV)
endif

KERN_SRCS := $(wildcard kernel/*.c) $(ARCH_SRCS) $(wildcard hal/*.c)
SRCS := $(KERN_SRCS) $(APP_SRCS)

# Per-board object tree (avoids stale cross-board .o)
OBJDIR := build/$(BOARD)
OBJS := $(addprefix $(OBJDIR)/,$(SRCS:.c=.o)) $(OBJDIR)/startup.o

# Default startup lives under arch/<ARCH>/；board.mk 可设 BOARD_STARTUP 覆盖
ifeq ($(strip $(BOARD_STARTUP)),)
  STARTUP_S := arch/$(BOARD_ARCH)/startup.S
else
  STARTUP_S := $(BOARD_STARTUP)
endif

.PHONY: all clean run run-demo test bench stress cli gdb info dump docs sdk qemu-cmd help

all: cgrtos.elf cgrtos.bin

$(OBJDIR)/startup.o: $(STARTUP_S) | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	@mkdir -p $@

cgrtos.elf: $(OBJS) $(LINK_SCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=cgrtos.map $(OBJS) $(LIBGCC) -o $@

cgrtos.bin: cgrtos.elf
	$(OBJCOPY) -O binary $< $@

# QEMU command fragments (also used by scripts/cgrtos.sh via make -s qemu-cmd)
QEMU_BASE := -M $(BOARD_QEMU_MACHINE) -smp $(CORES) -m $(BOARD_QEMU_MEM) \
	-cpu $(BOARD_QEMU_CPU)$(BOARD_QEMU_CPU_EXT) -nographic -serial mon:stdio
ifeq ($(BOARD_QEMU_NEED_LOADER),1)
  ifeq ($(CORES),2)
    QEMU_BASE += -device loader,addr=$(BOARD_QEMU_ENTRY),cpu-num=1
  else ifeq ($(CORES),4)
    QEMU_BASE += -device loader,addr=$(BOARD_QEMU_ENTRY),cpu-num=1 \
	-device loader,addr=$(BOARD_QEMU_ENTRY),cpu-num=2 \
	-device loader,addr=$(BOARD_QEMU_ENTRY),cpu-num=3
  endif
endif

qemu-cmd:
	@echo "QEMU=$(QEMU)"
	@echo "QEMU_LOAD=$(BOARD_QEMU_LOAD)"
	@echo "QEMU_ARCH=$(BOARD_ARCH)"
	@echo "QEMU_CROSS=$(CROSS)"
	@echo "QEMU_FLAGS=$(QEMU_BASE)"

run: cgrtos.bin
	./scripts/cgrtos.sh run --app $(APP) --cores $(CORES) --board $(BOARD) --no-build

run-demo:
	./scripts/cgrtos.sh demo --cores $(CORES) --board $(BOARD)

test:
	./scripts/cgrtos.sh test --cores $(CORES) --board $(BOARD)

bench:
	./scripts/cgrtos.sh bench --cores $(CORES) --board $(BOARD)

stress:
	./scripts/cgrtos.sh stress --cores $(CORES) --board $(BOARD)

cli:
	./scripts/cgrtos.sh cli --cores $(CORES) --board $(BOARD)

gdb:
	./scripts/cgrtos.sh gdb --app $(APP) --board $(BOARD)

docs:
	./scripts/cgrtos.sh docs

sdk:
	./scripts/cgrtos.sh sdk

debug: gdb

clean:
	rm -rf build/$(BOARD)
	rm -f cgrtos.elf cgrtos.bin cgrtos.map
	rm -rf sdk
	# Legacy in-tree objects (pre per-board build/)
	rm -f kernel/*.o arch/riscv/*.o arch/arm64/*.o hal/*.o demo/*.o tests/*.o cli/*.o startup.o

clean-all:
	rm -rf build sdk
	rm -f cgrtos.elf cgrtos.bin cgrtos.map
	rm -f kernel/*.o arch/riscv/*.o arch/arm64/*.o hal/*.o demo/*.o tests/*.o cli/*.o startup.o

help:
	@echo "CG-RTOS multi-arch build"
	@echo "  make BOARD=<board> [CPU=] [CORES=1|2|4] [APP=demo|test|stress|bench|cli]"
	@echo "Boards: nuclei_evalsoc riscv_virt sifive_u qemu_virt_a64"
	@echo "ARM64:  BOARD=qemu_virt_a64 CORES=1 CPU=cortex-a53|cortex-a72"
	@echo "Prefer: ./scripts/cgrtos.sh test|stress|demo --board …"
	@echo "See docs/PORTING.md and docs/SCRIPTS.md"

info:
	@echo "CG-RTOS BOARD=$(BOARD) ARCH=$(BOARD_ARCH) CPU=$(BOARD_QEMU_CPU)"
	@echo "APP=$(APP) CORES=$(CORES) PROFILE=$(PROFILE) OBJDIR=$(OBJDIR)"
	@echo "UART=$(BOARD_UART_DRV) LINK=$(LINK_SCRIPT) TIMER_HZ=$(BOARD_TIMER_HZ)"
	@echo "STARTUP=$(STARTUP_S) CROSS=$(CROSS)"
	@echo "QEMU=$(QEMU) LOAD=$(BOARD_QEMU_LOAD)"
	@echo "See docs/PORTING.md"

dump: cgrtos.elf
	$(OBJDUMP) -d cgrtos.elf | head -100
