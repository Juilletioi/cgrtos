# CG-RTOS — Nuclei UX900 / evalsoc (RISC-V64 SMP)
#
# Usage:
#   make                  # build demo
#   make APP=test         # build full-feature tests
#   make APP=bench        # build microbenchmarks
#   make test             # → ./scripts/cgrtos.sh test
#   make cli / stress / bench / docs
#   make sdk              # Doxygen + sdk/ package
# Prefer: ./scripts/cgrtos.sh <cmd>

CROSS   ?= riscv64-unknown-linux-gnu-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

SYSROOT ?= /home/cong/nuclei-ux900fd-linux/nuclei-linux-sdk/work/evalsoc/buildroot_initramfs/host/opt/ext-toolchain
QEMU    ?= /home/cong/nuclei-ux900fd-linux/tools/nuclei-qemu/bin/qemu-system-riscv64
export PATH := $(SYSROOT)/bin:$(PATH)

APP     ?= demo
ifeq ($(APP),test)
  APP_SRCS := tests/test_all.c tests/test_cases.c tests/stress_cases.c
else ifeq ($(APP),cli)
  APP_SRCS := tests/cli_main.c tests/test_cases.c tests/stress_cases.c
else ifeq ($(APP),bench)
  APP_SRCS := tests/bench_all.c
else ifeq ($(APP),stress)
  APP_SRCS := tests/stress_all.c tests/stress_cases.c
else
  APP_SRCS := $(wildcard demo/*.c)
endif

CFLAGS  := -march=rv64imafdc -mabi=lp64d -mcmodel=medany \
           -nostdlib -nostartfiles -ffreestanding -fno-builtin \
           -fno-stack-protector -O2 -g -Wall \
           -I$(SYSROOT)/riscv64-unknown-linux-gnu/sysroot/usr/include \
           -I. -Ikernel
LDFLAGS := -nostdlib -nostartfiles -ffreestanding -T cgrtos.lds
LIBGCC  := -lgcc

KERN_SRCS := $(wildcard kernel/*.c) $(wildcard arch/riscv/*.c)
SRCS := $(KERN_SRCS) $(APP_SRCS)
OBJS := $(SRCS:.c=.o) startup.o

.PHONY: all clean run run-demo test bench stress cli gdb info dump docs sdk

all: cgrtos.elf cgrtos.bin

startup.o: startup.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

cgrtos.elf: $(OBJS) cgrtos.lds
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=cgrtos.map $(OBJS) $(LIBGCC) -o $@

cgrtos.bin: cgrtos.elf
	$(OBJCOPY) -O binary $< $@

QEMU_FLAGS := -M nuclei_evalsoc,download=ddr -smp 2 -m 512M \
	-cpu nuclei-ux900fd,ext=svpbmt_zicbom_sstc_sscofpmf_zba_zbb_zbc_zbs_zicond \
	-nographic -serial mon:stdio \
	-device loader,addr=0xA0000000,cpu-num=1

run: cgrtos.bin
	./scripts/cgrtos.sh run --app $(APP) --no-build

run-demo:
	./scripts/cgrtos.sh demo

test:
	./scripts/cgrtos.sh test

bench:
	./scripts/cgrtos.sh bench

stress:
	./scripts/cgrtos.sh stress

cli:
	./scripts/cgrtos.sh cli

#   make APP=test gdb     # or: ./scripts/cgrtos.sh test --gdb
#   make APP=cli gdb
gdb:
	./scripts/cgrtos.sh gdb --app $(APP)

docs:
	./scripts/cgrtos.sh docs

sdk:
	./scripts/cgrtos.sh sdk

debug: gdb

clean:
	rm -f $(OBJS) kernel/*.o arch/riscv/*.o demo/*.o tests/*.o \
	      startup.o cgrtos.elf cgrtos.bin cgrtos.map
	rm -rf sdk

info:
	@echo "CG-RTOS for Nuclei UX900"
	@echo "APP=$(APP)"
	@echo "CC=$(CC)"
	@echo "SRCS=$(SRCS)"
	@echo "See docs/USER_GUIDE.md"

dump: cgrtos.elf
	$(OBJDUMP) -d cgrtos.elf | head -100
