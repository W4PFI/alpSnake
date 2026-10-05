#!/bin/bash
# Builds and runs ALP Snake's tests on your computer (macOS or Linux).
# None of them need the cabinet:
#   game_fuzz      random games against the rules engine, checking invariants
#   hawk_test      hawk attack scenarios
#   sign_test      SLOW signs and speed levels over long autopiloted games
#   frontend_test  the whole core inside a fake Retroplayer, with screenshots
#                  written to tests/out/*.ppm
# Usage: tests/run_tests.sh      (from anywhere; uses cc, i.e. clang or gcc)
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tests/out
CC=${CC:-cc}
# Address and undefined-behaviour sanitizers catch memory bugs the cabinet
# would only show as crashes. Set SANITIZE= to turn them off.
SANITIZE=${SANITIZE--fsanitize=address,undefined}
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Isrc -Iinclude $SANITIZE"

for test in game_fuzz hawk_test sign_test; do
    echo "== $test"
    $CC $FLAGS tests/$test.c src/snake_game.c -o tests/out/$test
    tests/out/$test
done

# The core itself, built in host-preview mode (no system calls, no SDL).
echo "== frontend_test"
$CC $FLAGS -DALP_SNAKE_HOST_PREVIEW -Wno-unused-function tests/frontend_test.c \
    src/snake_game.c src/snake_audio.c src/snake_smooth_font.c \
    -o tests/out/frontend_test -lm
tests/out/frontend_test
echo "All tests passed."
