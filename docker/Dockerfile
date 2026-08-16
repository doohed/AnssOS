# Build/run environment for AnssOS on a non-x86_64 host (an Apple Silicon
# Mac, specifically). Nothing in here is emulated: the image is native to
# the host architecture, because the two things that care about x86_64 can
# both be told about it without a foreign-arch userland. clang targets
# x86_64 from any host given --target (so no separate cross-gcc), and
# qemu-system-x86_64 emulates the guest CPU in TCG regardless of what it's
# running on. TCG means no KVM/HVF acceleration, which is fine for a kernel
# this size -- the whole thing is a few hundred KiB and boots in a second.
#
# Pinned to a dated Debian tag rather than a floating one so the toolchain
# doesn't drift underneath the build.
FROM debian:trixie-20260803-slim

# The package set is derived from what scripts/*.sh actually invoke:
#
#   build-iso.sh       -> make, $CC/$LD (clang + lld, see ENV below),
#                         xorriso, and git for the limine submodule
#   build-userland.sh  -> the same $CC/$LD, plus python3
#                         (gen-test-tone.py synthesizes a WAV fixture --
#                         real binary struct packing and float math, not
#                         a fit for bash -- see docs/play.md)
#   run-qemu.sh        -> qemu-system-x86_64, qemu-img, OVMF firmware
#   format.sh          -> clang-format
#
# disk-put.py (docs/play.md) is also python3 but isn't part of the build
# -- it writes straight onto AnssOS-disk.img from the host, run by hand.
#
# mtools is the README's documented dependency for the ISO step; xorriso
# builds the EFI boot partition itself here, but it's kept so the
# documented install line and this image stay the same set. nasm is *not*
# installed: the README lists it, but nothing in the tree is .asm -- the
# kernel's assembly is GAS .S files, assembled by $CC's own integrated
# assembler.
#
# ovmf is Architecture: all in Debian (the firmware blobs are just x86
# binaries in a data package), so it installs on arm64 unchanged and lands
# in /usr/share/OVMF/, which run-qemu.sh's split-firmware auto-detection
# already looks in.
RUN apt-get update && apt-get install -y --no-install-recommends \
        clang \
        lld \
        clang-format \
        make \
        git \
        ca-certificates \
        xorriso \
        mtools \
        qemu-system-x86 \
        qemu-utils \
        ovmf \
        python3 \
    && rm -rf /var/lib/apt/lists/*

# Self-configuring toolchain: both GNUmakefiles and build-userland.sh take
# CC/LD from the environment (defaulting to cc/ld for a native x86_64 host
# build), and kernel/GNUmakefile adds --target=x86_64-unknown-none only
# when CC is actually clang. Setting these here is all it takes to make the
# unmodified scripts cross-compile.
ENV CC=clang \
    LD=ld.lld

WORKDIR /work

CMD ["bash"]
