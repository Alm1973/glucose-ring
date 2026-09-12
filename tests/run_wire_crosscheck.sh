#!/bin/sh
# Cross-check -- a phone app's exact wire bytes, decoded by the firmware's own
# rp_parse() (tests/wire_crosscheck.cpp, unchanged).
#
# DISABLED. Its byte source was the Android app's WireExportTest, retired on
# branch ios-relay. A Swift exporter from the iOS app replaces it (see
# IOS_APP_REPORT.md). The Android version can still be run from history:
#   git restore --source=nightscout-relay -- android/    (needs the Android SDK)
#
# Exits non-zero on purpose: a check that cannot run must never look like a pass.
echo "run_wire_crosscheck.sh: DISABLED -- the Android exporter was removed; the iOS exporter is not written yet (IOS_APP_REPORT.md)." >&2
exit 2
