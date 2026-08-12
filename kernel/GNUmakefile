# Nuke built-in rules.
.SUFFIXES:

override OUTPUT := kernel

CC := cc
LD := ld

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

.PHONY: clean
clean:
	rm -rf bin $(OBJ) $(HEADER_DEPS)
