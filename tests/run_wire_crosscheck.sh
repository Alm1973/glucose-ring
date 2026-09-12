#!/bin/sh
# Cross-check -- no hardware. The iOS app's exact wire bytes, decoded by the
# firmware's own rp_parse(). Catches a formatter/parser disagreement that
# would otherwise look exactly like a dead link: the ring drops a malformed
# reading silently and never buzzes.
#
# Needs only Command Line Tools (swiftc + clang++). The Android version of the
# exporter is in git history (branch nightscout-relay).
set -e
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# 1. The app's own formatter (ios/Core) writes every wire line, after its own
#    tests pass.
if ! ios/run_core_tests.sh --export-wire "$OUT/wire" > "$OUT/core.log" 2>&1; then
  cat "$OUT/core.log"
  exit 1
fi
grep -E '^exported|^passed:' "$OUT/core.log"

# 2. Build the harness against the firmware's parser, same flags as run_tests.sh.
clang++ -std=c++17 -O1 -g \
  -Wall -Wextra -Wconversion -Wshadow -Wpedantic \
  -fsanitize=undefined -fno-omit-frame-pointer \
  -o "$OUT/wire_crosscheck" \
  tests/wire_crosscheck.cpp reading_parser.cpp

# 3. Decode every line; non-zero exit on any mismatch.
"$OUT/wire_crosscheck" "$OUT/wire/lines.bin" "$OUT/wire/expected.tsv"
