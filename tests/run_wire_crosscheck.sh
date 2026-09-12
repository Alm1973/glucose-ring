#!/bin/sh
# Cross-check -- no hardware. The Android app's exact wire bytes, decoded by
# the firmware's own rp_parse(). Catches a formatter/parser disagreement that
# would otherwise look exactly like a dead link: the ring drops a malformed
# reading silently and never buzzes.
#
# Needs the Android toolchain (PHONE_APP.md). run_tests.sh does not.
set -e
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

: "${ANDROID_HOME:=/opt/homebrew/share/android-commandlinetools}"
export ANDROID_HOME

# 1. The app's own formatter writes every wire line to android/app/build/wire/.
#    --rerun forces the export even when Gradle considers the test up to date.
(cd android && ./gradlew -q :app:testDebugUnitTest \
  --tests com.glucosering.ring.WireExportTest --rerun)

# 2. Build the harness against the firmware's parser, same flags as run_tests.sh.
clang++ -std=c++17 -O1 -g \
  -Wall -Wextra -Wconversion -Wshadow -Wpedantic \
  -fsanitize=undefined -fno-omit-frame-pointer \
  -o "$OUT/wire_crosscheck" \
  tests/wire_crosscheck.cpp reading_parser.cpp

# 3. Decode every line; non-zero exit on any mismatch.
"$OUT/wire_crosscheck" android/app/build/wire/lines.bin android/app/build/wire/expected.tsv
