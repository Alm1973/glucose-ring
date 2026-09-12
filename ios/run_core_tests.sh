#!/bin/sh
# The iOS app's pure core (ios/Core) and its tests, built with swiftc from
# Command Line Tools: no Xcode, no simulator, no device. XCTest is not in
# Command Line Tools, so the tests use a small runner in the style of
# tests/test_all.cpp.
#
#   ios/run_core_tests.sh                     run the tests
#   ios/run_core_tests.sh --export-wire DIR   also write wire bytes for
#                                             tests/run_wire_crosscheck.sh
set -e
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

swiftc -Onone -warnings-as-errors -o "$OUT/core_tests" Core/*.swift Tests/*.swift
"$OUT/core_tests" "$@"
