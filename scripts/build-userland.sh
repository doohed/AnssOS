#!/usr/bin/env bash
# Builds the tiny hand-rolled userland test payloads (hello, crash,
# malloctest, filetest, dirtest, forktest, forkchild, preempttest) used
# to prove the ring-3 pipeline end-to-end, and
# drops their built ELF binaries where kernel/src/exec/userland_blobs.S
# expects to find them (via .incbin) so they get embedded directly into
# the kernel image -- see main.c's self-test wiring for how they land on
# the VFS at boot.
#
# This is a genuinely separate build (its own toolchain flags, its own
# linker script) from the kernel itself -- these are ring-3 userland
# programs, not kernel code -- run before `make -C kernel` since the
# kernel's .incbin depends on its output.
set -euo pipefail
cd "$(dirname "$0")/.."

# Same CC/LD override as kernel/GNUmakefile, for the same reason: native
# cc/ld on an x86_64 host, clang/ld.lld when docker/Dockerfile's ENV says
# so. clang defaults to the host triple, so it needs telling that these
# are bare-metal x86_64 binaries; gcc gets nothing extra.
CC="${CC:-cc}"
LD="${LD:-ld}"
CFLAGS=(-g -O2 -ffreestanding -fno-stack-protector -fno-stack-check -fno-pic -fno-pie
    -ffunction-sections -fdata-sections -m64 -march=x86-64 -mabi=sysv -mno-80387 -mno-mmx
    -mno-sse -mno-sse2 -mno-red-zone)
if "$CC" --version 2>/dev/null | grep -qi clang; then
    CFLAGS+=(--target=x86_64-unknown-none)
fi
LDFLAGS=(-m elf_x86_64 -nostdlib -static --gc-sections -T userland/link.ld)

# The libc every program links against -- see userland/libc.h.
LIBC_SRCS=(crt0.S syscalls.c libc/string.c libc/malloc.c libc/stdio.c libc/termios.c libc/dirent.c)

build_program() {
    local name="$1"
    shift
    local objs=()
    for src in "${LIBC_SRCS[@]}" "$@"; do
        local obj="userland/${src%.*}.o"
        "$CC" "${CFLAGS[@]}" -c "userland/$src" -o "$obj"
        objs+=("$obj")
    done
    "$LD" "${LDFLAGS[@]}" "${objs[@]}" -o "userland/${name}.elf"
    cp "userland/${name}.elf" "kernel/src/exec/${name}.elf.bin"
}

build_program hello hello.c
build_program crash crash.c
build_program malloctest malloctest.c
build_program filetest filetest.c
build_program dirtest dirtest.c
build_program forktest forktest.c
build_program forkchild forkchild.c
build_program preempttest preempttest.c
build_program termtest termtest.c
build_program readdirtest readdirtest.c
build_program scarf scarf.c
build_program play play.c

# Not a userland ELF -- a synthesized WAV fixture for `play` (see the
# script itself for why), dropped at the same src/exec/*.bin location
# userland_blobs.S expects.
python3 "$(dirname "$0")/gen-test-tone.py"

echo "Built userland/{hello,crash,malloctest,filetest,dirtest,forktest,forkchild,preempttest,termtest,readdirtest,scarf,play}.elf"
