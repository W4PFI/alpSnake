#!/usr/bin/env bash
# build.sh - builds dist/ALP-Snake.UCE (or dist/ALP-Snake-Probe.UCE when run
# with ALP_SNAKE_PROBE=1).
#
# A thin wrapper: it checks that the Mac has everything the build needs, then
# runs tools/build.py, which compiles the game and packages the cartridge.
# Needs macOS (the artwork scripts use Swift/AppKit and the sips image tool),
# plus Homebrew's llvm (clang), lld (Linux linker), squashfs (mksquashfs)
# and e2fsprogs (mke2fs, debugfs).
#
# -e stop at the first failing command, -u treat unset variables as errors,
# -o pipefail a pipeline fails if any part of it fails.
set -euo pipefail

# Work from the project folder, wherever the script was started from.
cd "$(dirname "$0")"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This build currently needs macOS for the artwork tools (Swift and sips)." >&2
    exit 1
fi

# Command-line tools that must be on PATH.
for command in brew python3 swift sips; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Missing $command. Install Xcode command-line tools and Homebrew first." >&2
        exit 1
    fi
done

# Homebrew packages: collect every missing one so a single message can list them.
missing=()
for formula in llvm lld squashfs e2fsprogs; do
    if ! brew --prefix "$formula" >/dev/null 2>&1; then
        missing+=("$formula")
    fi
done
if (( ${#missing[@]} )); then
    echo "Missing Homebrew packages: ${missing[*]}" >&2
    echo "Install them with: brew install ${missing[*]}" >&2
    exit 1
fi

python3 tools/build.py
# Report which file was produced (the probe build has a different name).
echo "Ready: dist/$( [ "${ALP_SNAKE_PROBE:-}" = 1 ] && echo ALP-Snake-Probe.UCE || echo ALP-Snake.UCE )"
