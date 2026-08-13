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

exec qemu-system-x86_64 \
    -M q35 \
    -m 256M \
    -no-shutdown \
    -vga none \
    "${fw_args[@]}" \
    -cdrom "$ISO" \
    -device virtio-gpu-pci \
    -device virtio-keyboard-pci \
    -drive file="$DISK",if=none,id=disk0,format=raw \
    -device virtio-blk-pci,drive=disk0,disable-legacy=on \
    -serial stdio \
    "$@"
