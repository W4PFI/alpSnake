#!/bin/bash
# Builds ALP-Snake.UCE and copies it to the root of the UNTITLED USB key.
#
# A ".command" file is a shell script macOS opens in Terminal when you
# double-click it in Finder, so the whole build-and-install can be done
# without typing. The cabinet then loads the .UCE from the USB stick.
set -euo pipefail
cd "$(dirname "$0")"
# Where macOS mounts the USB stick (a stick named UNTITLED).
USB=/Volumes/UNTITLED
# Finder-launched scripts get a minimal PATH; add Homebrew's locations
# (Apple silicon and Intel Macs).
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

./build.sh

if [[ ! -d "$USB" ]]; then
    echo "USB key not mounted at $USB" >&2
    exit 1
fi
# The saved high scores and settings live INSIDE the .UCE (its ext4 save
# image), so overwriting it resets them. Keep a timestamped copy of the old
# file in dist/ first so nothing is lost.
if [[ -f "$USB/ALP-Snake.UCE" ]]; then
    backup="dist/ALP-Snake-from-USB-$(date +%Y%m%d-%H%M%S).UCE"
    cp "$USB/ALP-Snake.UCE" "$backup"
    echo "Backed up previous USB copy (with its saved best score) to $backup"
fi
cp dist/ALP-Snake.UCE "$USB/ALP-Snake.UCE"
# Flush the copy to the stick before it is unplugged.
sync
echo
# Matching checksums confirm the stick holds exactly the file just built.
echo "Built:  $(shasum -a 256 dist/ALP-Snake.UCE)"
echo "On USB: $(shasum -a 256 "$USB/ALP-Snake.UCE")"
ls -la "$USB"
echo "DONE"
