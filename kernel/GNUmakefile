# Nuke built-in rules.
.SUFFIXES:

override OUTPUT := kernel

# Overridable from the environment so a cross-build works without editing
# anything here: on an x86_64 Linux host these stay the native cc/ld, and
# docker/Dockerfile sets CC=clang / LD=ld.lld for a non-x86_64 host.
CC ?= cc
LD ?= ld

# User controllable flags.
CFLAGS := -g -O2 -pipe -Wall -Wextra -std=gnu11
CPPFLAGS := -I src

# Internal flags that should not be changed by the user: this is a
# freestanding x86_64 long-mode kernel loaded by Limine, so no libc,
# no red zone (interrupts can land on top of the stack otherwise), no
# SIMD (would need explicit FPU/SSE state management we don't have yet).
override CFLAGS += \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fno-PIC \
    -ffunction-sections \
    -fdata-sections \
    -m64 \
    -march=x86-64 \
    -mabi=sysv \
    -mno-80387 \
    -mno-mmx \
    -mno-sse \
    -mno-sse2 \
    -mno-red-zone \
    -mcmodel=kernel

# gcc only ever builds for the machine it was built for, so a native
# x86_64 gcc needs nothing extra and gets nothing extra -- the flags above
# are exactly what it saw before. clang is a cross-compiler by
# construction, though, and defaults to the *host* triple, so on a non-x86
# host it has to be told what we're building for. -unknown-none (rather
# than -linux-gnu) is the bare-metal triple: no OS, no libc assumptions,
# which is what -ffreestanding above is already asking for anyway.
ifneq (,$(findstring clang,$(shell $(CC) --version 2>/dev/null)))
override CFLAGS += --target=x86_64-unknown-none
endif

override CPPFLAGS += -MMD -MP

override LDFLAGS := \
    -m elf_x86_64 \
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    --gc-sections \
    -T linker.ld

CFILES := $(shell find src -type f -name '*.c')
ASFILES := $(shell find src -type f -name '*.S')
OBJ := $(CFILES:.c=.c.o) $(ASFILES:.S=.S.o)
HEADER_DEPS := $(CFILES:.c=.c.d) $(ASFILES:.S=.S.d)

.PHONY: all
all: bin/$(OUTPUT)

bin/$(OUTPUT): GNUmakefile linker.ld $(OBJ)
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $(OBJ) -o $@

-include $(HEADER_DEPS)

%.c.o: %.c GNUmakefile
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

%.S.o: %.S GNUmakefile
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# .incbin isn't an #include, so -MMD never records the embedded userland
# payloads as dependencies -- editing a userland program and rebuilding
# would quietly keep the *previous* binary in the kernel image, since
# userland_blobs.S itself hadn't changed. The blobs exist by the time
# this is parsed: build-iso.sh runs scripts/build-userland.sh first.
src/exec/userland_blobs.S.o: $(wildcard src/exec/*.elf.bin)

.PHONY: clean
clean:
	rm -rf bin $(OBJ) $(HEADER_DEPS)
