#!/usr/bin/env bash
# Applies (or checks) the project's code style -- see .clang-format at the
# repo root -- across every kernel .c/.h file. (clang-format doesn't
# understand GAS assembly, so isr.S etc. are left alone.)
#
# Usage:
#   ./scripts/format.sh          # reformat in place
#   ./scripts/format.sh --check  # exit non-zero if anything isn't formatted
#                                 # (doesn't modify files -- CI-friendly)
#
# Requires: clang-format.
#   sudo apt-get install -y clang-format
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v clang-format >/dev/null 2>&1; then
    echo "error: clang-format not found. Install it with:" >&2
    echo "  sudo apt-get install -y clang-format" >&2
    exit 1
fi

# font8x8_basic.h and boot/limine.h are vendored as-is from upstream (see
# their own header comments) -- never reformat vendored code.
mapfile -t FILES < <(
    find kernel/src -type f \( -name '*.c' -o -name '*.h' \) \
        -not -name 'font8x8_basic.h' \
        -not -name 'limine.h' \
        | LC_ALL=C sort
)

if [ "${1:-}" = "--check" ]; then
    fail=0
    for f in "${FILES[@]}"; do
        if ! clang-format --dry-run --Werror "$f" > /dev/null 2>&1; then
            echo "not formatted: $f"
            fail=1
        fi
    done
    if [ "$fail" -ne 0 ]; then
        echo "Run ./scripts/format.sh to fix." >&2
        exit 1
    fi
    echo "All ${#FILES[@]} files formatted correctly."
else
    clang-format -i "${FILES[@]}"
    echo "Formatted ${#FILES[@]} files."
fi
