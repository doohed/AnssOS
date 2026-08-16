#!/usr/bin/env bash
# Boots AnssOS.iso in QEMU over UEFI, virtio-gpu as the only display
# device. Does NOT build the ISO -- run scripts/build-iso.sh (or `make`)
# first.
#
# Requires: qemu-system-x86_64, OVMF firmware.
#   sudo apt-get install -y qemu-system-x86 ovmf
set -euo pipefail
cd "$(dirname "$0")/.."

ISO="AnssOS.iso"
if [ ! -f "$ISO" ]; then
    echo "error: $ISO not found -- run ./scripts/build-iso.sh (or \`make\`) first" >&2
    exit 1
fi

# Persistent storage backing (M9, virtio-blk) -- a blank disk formats
# itself on first `sync`/auto-save from the shell. Kept out of git
# (see .gitignore) since it's runtime state, not source.
DISK="AnssOS-disk.img"
if [ ! -f "$DISK" ]; then
    echo "Creating blank $DISK (16 MiB)..."
    qemu-img create -f raw "$DISK" 16M >/dev/null
fi

find_ovmf_combined() {
    for p in /usr/share/OVMF/OVMF.fd /usr/share/ovmf/OVMF.fd; do
        [ -f "$p" ] && printf '%s\n' "$p" && return 0
    done
    return 1
}

find_ovmf_split() {
    local candidates=(
        "/usr/share/OVMF/OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd"
        "/usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd"
        "/usr/share/edk2/x64/OVMF_CODE.4m.fd:/usr/share/edk2/x64/OVMF_VARS.4m.fd"
        # Homebrew's qemu ships its own edk2 build, so a Mac can run this
        # script directly rather than only inside the container. Note the
        # names don't match: there is no edk2-x86_64-vars.fd, the i386
        # vars file is the correct pair for the x86_64 code blob.
        "/opt/homebrew/share/qemu/edk2-x86_64-code.fd:/opt/homebrew/share/qemu/edk2-i386-vars.fd"
        "/usr/local/share/qemu/edk2-x86_64-code.fd:/usr/local/share/qemu/edk2-i386-vars.fd"
    )
    for pair in "${candidates[@]}"; do
        local code="${pair%%:*}" vars="${pair##*:}"
        if [ -f "$code" ] && [ -f "$vars" ]; then
            printf '%s\n' "$pair"
            return 0
        fi
    done
    return 1
}

fw_args=()
if combined=$(find_ovmf_combined); then
    fw_args=(-bios "$combined")
elif split=$(find_ovmf_split); then
    code="${split%%:*}"
    vars="${split##*:}"
    vars_local="$(pwd)/OVMF_VARS.local.fd"
    [ -f "$vars_local" ] || cp "$vars" "$vars_local"
    fw_args=(
        -drive "if=pflash,format=raw,readonly=on,file=$code"
        -drive "if=pflash,format=raw,file=$vars_local"
    )
else
    echo "error: OVMF UEFI firmware not found. Install it with:" >&2
    echo "  sudo apt-get install -y ovmf" >&2
    exit 1
fi

# Audio backend (M17, virtio-sound) -- auto-picks a real, audible
# backend when one is actually reachable: CoreAudio on macOS,
# PulseAudio/PipeWire via `pactl` then ALSA via `aplay` on Linux, so
# `play` just comes out of your speakers by default. Falls back to
# QEMU's "wav" backend (captures to AnssOS-audio-out.wav, gitignored
# like AnssOS-disk.img, instead of speakers) only when none is
# reachable -- e.g. a headless CI/sandbox box, or the build container,
# which has no audio setup at all -- so a boot never hard-fails here for
# lack of sound hardware. Set QEMU_AUDIODEV yourself to override, e.g.
# to force the wav capture back on for a deterministic test:
# QEMU_AUDIODEV="wav,id=snd0,path=AnssOS-audio-out.wav".
if [ -n "${QEMU_AUDIODEV:-}" ]; then
    : # explicit override wins, nothing to detect
elif [ "$(uname -s)" = "Darwin" ]; then
    # macOS has neither pactl nor aplay, so without this it fell through
    # to the wav fallback below and captured to a file instead of making
    # any sound -- which looks exactly like broken audio.
    QEMU_AUDIODEV="coreaudio,id=snd0"
elif command -v pactl >/dev/null 2>&1 && pactl info >/dev/null 2>&1; then
    QEMU_AUDIODEV="pa,id=snd0"
elif command -v aplay >/dev/null 2>&1 && aplay -l >/dev/null 2>&1; then
    QEMU_AUDIODEV="alsa,id=snd0"
else
    QEMU_AUDIODEV="wav,id=snd0,path=AnssOS-audio-out.wav"
fi

exec qemu-system-x86_64 \
    -M q35 \
    -m 512M \
    -no-shutdown \
    -vga none \
    "${fw_args[@]}" \
    -cdrom "$ISO" \
    -device virtio-gpu-pci \
    -device virtio-keyboard-pci \
    -drive file="$DISK",if=none,id=disk0,format=raw \
    -device virtio-blk-pci,drive=disk0,disable-legacy=on \
    -device virtio-sound-pci,audiodev=snd0 \
    -audiodev "$QEMU_AUDIODEV" \
    -serial stdio \
    "$@"
