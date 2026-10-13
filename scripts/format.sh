#!/usr/bin/env bash
# Applies (or checks) the kernel's code style -- rustfmt, with the
# settings in kernel/rustfmt.toml -- across the kernel crate. Generated
# tables (console/font.rs) opt out with #[rustfmt::skip].
#
# Usage:
#   ./scripts/format.sh          # reformat in place
#   ./scripts/format.sh --check  # exit non-zero if anything isn't formatted
#                                 # (doesn't modify files -- CI-friendly)
#
# Requires: rustfmt (rustup component add rustfmt).
set -euo pipefail
cd "$(dirname "$0")/../kernel"

if ! cargo fmt --version >/dev/null 2>&1; then
    echo "error: rustfmt not found. Install it with:" >&2
    echo "  rustup component add rustfmt" >&2
    exit 1
fi

if [ "${1:-}" = "--check" ]; then
    if ! cargo fmt --check >/dev/null; then
        cargo fmt --check -- --files-with-diff | sed 's/^/not formatted: /'
        echo "Run ./scripts/format.sh to fix." >&2
        exit 1
    fi
    echo "The kernel is formatted correctly."
else
    cargo fmt
    echo "Formatted the kernel."
fi
