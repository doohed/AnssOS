#!/usr/bin/env bash
# Builds the tiny hand-rolled userland test payloads (hello, crash) used
# to prove M10's ring-3 pipeline end-to-end, and drops their built ELF
# binaries where kernel/src/exec/userland_blobs.S expects to find them
# (via .incbin) so they get embedded directly into the kernel image --
# see main.c's M10 self-test wiring for how they land on the VFS at boot.
#
# This is a genuinely separate build (its own toolchain flags, its own
# linker script) from the kernel itself -- these are ring-3 userland
# programs, not kernel code -- run before `make -C kernel` since the
# kernel's .incbin depends on its output.
set -euo pipefail
cd "$(dirname "$0")/.."

CC=cc
LD=ld
CFLAGS=(-g -O2 -ffreestanding -fno-stack-protector -fno-stack-check -fno-pic -fno-pie -m64
    -march=x86-64 -mabi=sysv -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone)
LDFLAGS=(-m elf_x86_64 -nostdlib -static -T userland/link.ld)

build_program() {
    local name="$1"
    shift
    local objs=()
    for src in "$@"; do
        local obj="userland/${src%.*}.o"
        "$CC" "${CFLAGS[@]}" -c "userland/$src" -o "$obj"
        objs+=("$obj")
    done
    "$LD" "${LDFLAGS[@]}" "${objs[@]}" -o "userland/${name}.elf"
    cp "userland/${name}.elf" "kernel/src/exec/${name}.elf.bin"
}

build_program hello crt0.S syscalls.c hello.c
build_program crash crt0.S syscalls.c crash.c

echo "Built userland/hello.elf and userland/crash.elf"
