#!/bin/bash
# Builds the diagnostic ALP-Snake-Probe.UCE and copies it to the UNTITLED USB key.
# It does not touch the regular ALP-Snake.UCE or its saved scores.
#
# The probe build presents itself to Retroplayer like the real game but logs
# what Retroplayer does (save/probe.log) and, through its exec.sh, copies
# Retroplayer's output and system information into an alp-probe folder on the
# USB stick. Double-click it in Finder to run it in Terminal. See README.
set -euo pipefail
cd "$(dirname "$0")"
# Where macOS mounts the USB stick, and Homebrew's locations for the minimal
# PATH that Finder-launched scripts get.
USB=/Volumes/UNTITLED
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

ALP_SNAKE_PROBE=1 ./build.sh

if [[ ! -d "$USB" ]]; then
    echo "USB key not mounted at $USB" >&2
    exit 1
fi
cp dist/ALP-Snake-Probe.UCE "$USB/ALP-Snake-Probe.UCE"
# Flush the copy to the stick before it is unplugged.
sync
echo
echo "Copied: $(shasum -a 256 "$USB/ALP-Snake-Probe.UCE")"
echo "DONE"
