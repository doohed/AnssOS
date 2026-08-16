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
# backend when one is actually reachable (PulseAudio/PipeWire via
# `pactl`, else ALSA via `aplay`), so `play` just comes out of your
# speakers by default. Falls back to QEMU's "wav" backend (captures to
# AnssOS-audio-out.wav, gitignored like AnssOS-disk.img, instead of
# speakers) only when neither is reachable -- e.g. a headless CI/sandbox
# box with no audio setup at all -- so a boot never hard-fails here for
# lack of sound hardware. Set QEMU_AUDIODEV yourself to override either
# way, e.g. QEMU_AUDIODEV="coreaudio,id=snd0" on macOS, or force the wav
# capture back on for a deterministic test: QEMU_AUDIODEV="wav,id=snd0,path=AnssOS-audio-out.wav".
if [ -n "${QEMU_AUDIODEV:-}" ]; then
    : # explicit override wins, nothing to detect
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
