#!/bin/sh
# Host test harness -- no Arduino, no hardware.
set -e

# NOTE: -fsanitize=address is deliberately absent. Apple clang 14.0.3 ships an
# ASan runtime that aborts at init on this macOS (Darwin 25.x):
#   "CHECK failed: sanitizer_malloc_mac.inc ... asan_init_is_running"
# UBSan works. The tests also carry explicit sentinel-byte bounds checks so
# buffer overruns are caught without ASan. Re-add it on a newer toolchain.
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

clang++ -std=c++17 -O1 -g \
  -Wall -Wextra -Wconversion -Wshadow -Wpedantic \
  -fsanitize=undefined -fno-omit-frame-pointer \
  -o "$OUT/test_all" \
  tests/test_all.cpp reading_parser.cpp ring_config.cpp command.cpp liveness.cpp

"$OUT/test_all"
