#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Demo/stress: draw random strings on a framebuffer forever, to watch a live
# display or usbmon traffic.  This is NOT part of run_tests.py -- it has no
# oracle and never reports PASS/FAIL.
#
#   tests/random_str.sh [fb-index]
#
# Needs /dev/fbN on the machine running it (board or `make qemu` guest).
HERE=$(cd "$(dirname "$0")" && pwd)
FBCTL=$HERE/../tools/fbctl
FB=${1:-0}

[ -x "$FBCTL" ] || { echo "build it first: make -C $HERE/../tools" >&2; exit 3; }

# Read the real geometry instead of assuming a resolution.
read -r SCREEN_W SCREEN_H < <("$FBCTL" info --fd "$FB" --json \
  | python3 -c 'import json,sys; d=json.load(sys.stdin)["observation"]; print(d["xres"], d["yres"])') || exit 3

rand_str() {
    local len=$((20 + RANDOM % 60))
    tr -dc 'A-Za-z0-9' </dev/urandom | head -c "$len"
}

while true; do
    X=$((RANDOM % SCREEN_W))
    Y=$((RANDOM % SCREEN_H))
    "$FBCTL" text "$X" "$Y" "$(rand_str)" --fd "$FB" --quiet
    sleep 0.1
done
