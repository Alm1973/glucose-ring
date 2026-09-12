# iOS relay — report

**Status (2026-09-12): Phases 0–2 done and committed; Phase 3 written and
tested as far as possible without Xcode.** Everything is on branch
`ios-relay`: not merged into `main`, not pushed (there is no remote).

| Phase | Result | Commit |
|---|---|---|
| 0 Audit | Reported, stopped for approval | (this file, first version) |
| 1 Remove Android | 22 files `git rm`'d; SDK, `~/.gradle`, `~/.android` removed; **2.14 GiB reclaimed** | `7cd58df` |
| 2 Firmware: ring pokes | protocol 3, MISS alarm, trusted clock; **450/0 host tests** | `64731af` |
| 3 iOS app | Swift core **112/0**, wire cross-check **27,888/27,888**, whole app typechecks (Catalyst); **not built for iOS** | this commit |

**Nothing has run on the ring or on a phone.** The ring hardware's status is
unknown, and Xcode is not installed.

---

## 1. Done — and how each was verified

### Phase 0 — audit
- **Disk (measured):** Android SDK 524M, `~/.gradle` 1.4G, `~/.android` 3.5M,
  repo `android/` 26M (24M of it build output), `~/Library/Android` absent.
- **Reuse audit:** file by file. Design that survived: the staleness,
  future-date and validity rules; strict parsing; dedupe; token-safe errors;
  "Relaying only when the ring confirms"; the MTU chunking rule. The platform
  code is gone.
- **Safety net:** `nightscout-relay` held all 22 app files, and the tree was
  clean. `main` had never been merged: it still held the xDrip+ app.
- **Toolchain:** no Xcode. Command Line Tools 14.3.1 (Swift 5.8.1), with no
  iOS SDK and no XCTest.
- **Your decisions:** fast-forward `main`, then branch; remove the SDK,
  `~/.gradle` and `~/.android` only; poke with `#REQ` on the existing TX
  characteristic; sign with a free Personal Team.

### Phase 1 — Android removed (`7cd58df`)
- `main` was fast-forwarded to `686093e`, so it now holds the Nightscout app
  in history. `ios-relay` was cut from it, and `git rm -r android/` removed
  22 files.
- **Restore command** (22 files, verified recoverable):
  ```bash
  cd ~/projects/glucose-ring && git restore --source=nightscout-relay -- android/
  ```
- **Uninstalled:**
  - `brew uninstall --cask android-commandlinetools`. It left 351 MiB of
    `sdkmanager`-installed packages in the SDK folder, which I also removed.
  - `~/.gradle` and `~/.android`.
  - I first stopped the Gradle daemon that was still running from earlier
    builds.
  - Verified: all paths gone; `sdkmanager`, `avdmanager` and `adb` no longer
    on PATH.
- **Disk reclaimed, measured:** `df` free space went from 10,143,392 KiB
  (9.67 GiB) to 12,388,532 KiB (11.81 GiB). That's **+2.14 GiB**. `du` of what
  I removed sums to 2,015,608 KiB (1.92 GiB). The extra ~224 MiB is **not
  explained**; possibly Homebrew purging its download cache ("Purging files"
  in its output), possibly unrelated disk activity. I didn't attribute it.
- **Left in place, per your call:**
  - under `android/` (gitignored): `local.properties` (**contains your live
    token**), `app/build` 24M, `.gradle` 1.2M, `build` 132K, `.kotlin`;
  - Homebrew `gradle` and `openjdk` (605M);
  - the JDKs.

  `.gitignore` keeps its `android/` lines deliberately, so that token file can
  never be committed by accident.
- `PHONE_APP.md` now describes the iOS design, with the requirements carried
  over. `PHONE_APP_REPORT.md` is kept as the Android record, with a "retired"
  banner.

### Phase 2 — the ring drives the cycle (`64731af`)
- **Poke:** every ~5 min the ring sends `#REQ` on the **existing NUS TX
  characteristic**, so there is no new UUID. It sends at once on every fresh
  trusted link too. It only sends when the link is trusted and the phone has
  subscribed (`bleuart.notifyEnabled()`).
- **Miss counting** (`liveness.cpp`, pure):
  - A poke not answered by a valid `!TIME` within 30 s is a miss.
  - A poke that falls due with **no usable link** is a miss at once, so an
    out-of-range phone is caught too.
  - After `LV_MISS_ALERT_COUNT` = 3 in a row (a named constant), the ring is
    blind and buzzes **MISS**: 2 × 1000 ms at full drive. The pattern is fixed
    and refused as a low/high/stale pattern.
  - Priority: a current low/high, then MISS, then STALE, then none. **A blind
    ring is never silent.**
- **Trusted time:** the new `!TIME <epoch_ms>` gives the ring wall time.
  Readings are judged by **their own timestamp** against it, never by when
  they arrived.
  - No `!TIME` since boot means no reading is accepted.
  - More than 15 min old, or more than 2 min in the future, means dropped.
  - This fixes the protocol-2 flaw where a stale first reading after a reboot
    looked fresh.
- **Alignment:** the next poke is aimed at `reading + 5 min + 2 min`, clamped
  to between 1 and 5 minutes from now, so it can never be rarer than the
  fixed cadence.
- `PROTOCOL.md` was updated in the same commit (protocol 3, new §9), along
  with the README (MISS alarm, bring-up checklist steps 18–22). fw 3.0.0,
  proto 3; the config schema is unchanged; `#STATUS` is unchanged.
- **Verified:**
  - **Host tests: 450 passed, 0 failed** (from 343; no existing assertion
    changed). They include:
    - exhaustive checks that the ring is **never silent once blind** and
      **never acts on data that isn't current**;
    - simulated time-to-MISS at 1 s loop resolution: within 15.5 min whether
      the phone is connected-but-silent or out of range;
    - the pinned default of 3 × 5 min.
  - **Mutation test:** six deliberate breaks of the safety logic, each
    **caught**, plus a positive control that passes. The breaks were: drop
    "blind → MISS"; low/high on stale data; link-down poke not counted;
    no-clock treated as fresh; miss count raised to 4; `!TIME` floor removed.
  - **Firmware compiles** in both the default and the
    `TEST_MODE`/`DEBUG_SERIAL` builds: 137,292 bytes flash (16%), 14,660 bytes
    RAM. The **only warning in the whole build**, core included, is the
    existing placeholder-threshold `#warning`.

### Phase 3 — iOS app (this commit)
- **`ios/Core`** (pure, Foundation only):
  - `RingProtocol`: wire format, direction hygiene, `!TIME`, ring-line
    parsing, reassembly, chunking.
  - `Nightscout`: strict parsing. Whole numbers only; ms epoch; booleans and
    strings refused.
  - `Relay`/`RelayState`: only current, valid readings are written; dedupe
    per link; a **sticky "token rejected"** that a network blip cannot clear;
    honest status; `Backoff`.
- **`ios/GlucoseRing`** (the app):
  - `CBCentralManager` with `CBCentralManagerOptionRestoreIdentifierKey`.
    `centralManager(_:willRestoreState:)` **reattaches to the restored
    peripheral and never rescans**. The central is created in
    `didFinishLaunching` via `UIApplicationDelegateAdaptor`, so it exists on a
    background relaunch.
  - Info.plist sets `UIBackgroundModes = bluetooth-central` and
    `NSBluetoothAlwaysUsageDescription`.
  - **On `#REQ`:**
    1. `!TIME` is written first;
    2. then one GET (5 s timeout, no retry);
    3. then the reading is written back;
    4. then `!GET STATUS` confirms it.

    It runs inside `beginBackgroundTask`.
  - **401/403 is shown as "Token rejected"**, distinct from "Can't reach
    Nightscout", plus one local notification. It stays until a fetch
    succeeds.
  - **Reconnect:** an immediate, non-expiring `connect()`. Only
    connect-then-drop loops are paced, and never by more than 8 s, inside a
    background task.
  - Writes are chunked to MTU − 3. The ring's RX characteristic is sized to
    MTU 23 (`BLEUart.cpp:179`; `BANDWIDTH_LOW` = default MTU,
    `bluefruit.cpp:232`), so a long write would be rejected.
- **Secrets:** `ios/GlucoseRing/Secrets.plist`. `git check-ignore` confirmed
  it was ignored (`.gitignore:28`) **before** it was written. It holds the
  placeholder, and git does not list it.
- **`tests/run_wire_crosscheck.sh` is re-enabled.** It is now fed by the Swift
  exporter and needs only Command Line Tools.
- **Verified:**
  - **Swift core tests: 112 passed, 0 failed**, built with
    `-warnings-as-errors`. They include the live Nightscout body as a fixture,
    every truncation of it, and an exhaustive 180-combination check that
    "Relaying" appears for exactly one combination.
  - **Mutation test:** six deliberate breaks, each **caught**, plus a positive
    control. The breaks were: stale readings sent; future-dated sent; a
    network blip clearing 401; "Relaying" without the ring's confirmation;
    `"204"` coerced; commas kept.
  - **Wire cross-check:** the Swift formatter's 27,888 lines (829,314 bytes),
    fed in 20-byte chunks through the firmware's real `rp_parse()`, **all
    decode exactly**.
  - **Whole app typechecks** against the Mac Catalyst SDK
    (`arm64-apple-ios16.0-macabi`, Command Line Tools' macOS 13.3 SDK).
    That covers CoreBluetooth, restoration, UIKit, SwiftUI and
    UserNotifications, with no errors or warnings. **This is not an iOS
    build.**
  - `Info.plist` and `project.pbxproj` pass `plutil -lint`. The project
    parses to 17 objects with no dangling references.
  - The firmware host suite is still 450/0.

## 2. Not done

| Item | Blocked on |
|---|---|
| Build for iOS, open the Xcode project, install on the iPhone | **Xcode is not installed.** You must install it: it needs your Apple ID sign-in, license acceptance and `sudo xcode-select`. **Disk: 11.81 GiB free. Plan for roughly 20–25 GB** (estimate, not measured). Build steps: PHONE_APP.md §5. |
| Real token in the app | You, pasting into `ios/GlucoseRing/Secrets.plist` |
| Anything on the ring | Hardware. You didn't say whether it has arrived. |
| **Measure** the added sleep current | Hardware + µA meter (README checklist step 11). Section 6 is a calculation. |
| Deployment target | Set to iOS 17.0 as a guess; I don't know your iPhone's iOS version. |
| Tuning `LV_ALIGN_LAG_MS` (2 min) | Real arrival-lag data; there's only one sample |
| The app following a changed ring stale window (`!SET STALE`) | Not built. The app uses 15 min for its own checks; the ring is authoritative. |
| Deleting `android/` leftovers, including `local.properties` with your live token | Your call. I recommend deleting it once the token is in `Secrets.plist`. |
| Merging `ios-relay` into `main` / a remote backup | Your call. **This `.git` directory is the only copy of the history.** |

## 3. Assumptions — inferred, not read

1. **iOS pairs when the ring sends its Security Request.** iOS has no app API
   to start bonding, and the NUS characteristics are open, so nothing else
   triggers pairing. The ring ignores unencrypted links. **If iOS does not
   honour the request, the app never gets data.** This is the top risk; check
   it with nRF Connect on the iPhone as soon as a ring exists.
2. iOS wakes the suspended app for a notification with roughly 10 s of
   execution; `beginBackgroundTask` extends that a little. From memory of
   Apple's documentation; not measured.
3. State restoration relaunches the app after a **system** kill; **not after
   a force-quit**, and not after a reboot until the app is opened. The ring's
   MISS alarm is the backstop.
4. With the SwiftUI lifecycle, `UIApplicationDelegateAdaptor`'s
   `didFinishLaunching` runs on a background relaunch, so the central is
   created in time.
5. A free Personal Team allows the `bluetooth-central` background mode. Its
   **7-day expiry** means reinstalling weekly; when you forget, the ring
   buzzes MISS.
6. The bundle ID `com.glucosering.relay` is free for your team. It may need
   changing.
7. `maximumWriteValueLength(for: .withoutResponse)` reports MTU − 3 (20 here),
   while `.withResponse` would report 512.
8. The project uses the `objectVersion 77` synchronized-folder format as I
   remember it from Xcode 16. It is linted, but never opened.
9. A clean Catalyst typecheck implies a clean iOS compile. Availability can
   differ between the two (for example restoration symbols); not proven.
10. iOS Foundation's JSONSerialization tells integers, decimals and booleans
    apart the same way macOS 26's does (the tests ran on macOS).
11. `LV_ALIGN_LAG_MS` = 2 min is a guess; the only data point is one reading
    fetched 2.4 min after it was taken.
12. MISS shares the stale repeat cap (6 buzzes, about 30 min, then the
    "gave up" pattern), extending your earlier cap decision to it.
    `!SET STALEREPEATS 0` disables the cap for both.
13. MISS (2 × 1000 ms) can be told apart from low (3 × 400 ms) on a finger.
    Ergonomics; untested.
14. All battery figures in section 6 are calculated, not measured.

## 4. Discrepancies

1. **"315 assertions."** Actually 343 at the start; now 450.
2. **`main` had never been merged** and held the xDrip+ app. Resolved by your
   fast-forward decision.
3. **"Add a request characteristic" vs "do not invent UUIDs."** A new
   characteristic needs a new UUID. Resolved by your choice: `#REQ` on the
   existing TX notify characteristic.
4. **"Keep the existing reading characteristic."** There is no separate one:
   readings and commands share NUS RX, which is the write-back channel.
5. **"RTC wake… the budget assumed the ring was passive."** The ring was
   never asleep between readings. `loop()` already wakes every 1 s and holds
   a live connection. The poke is a timer check in that loop, with no new
   wake source.
6. **"Measure the added sleep current."** Impossible without hardware;
   calculated instead (section 6).
7. **"Use timestamps, don't trust arrival time."** The ring has no clock, so
   this needed `!TIME`. That is a **breaking** protocol change: readings
   without a prior `!TIME` are now dropped, and the bring-up checklist was
   updated to match. Any protocol-2 client, such as the retired Android app,
   no longer works.
8. **"Reconnect with backoff."** On iOS the idiom is an immediate,
   non-expiring `connect()`; an app timer can't run while suspended. Backoff
   is used only for connect-then-drop loops, capped at 8 s so it can't strand
   the app.
9. **"If the fetch fails, let the ring's miss counter do its job."** The app
   sends `!TIME` **before** it fetches, so a failed fetch shows on the ring as
   **STALE**, and **MISS** means the phone, app or link itself is gone. The
   ring would otherwise make a revoked token and a dead phone buzz
   identically. This is deliberate and reversible on the app side; say if you
   want a failed fetch to count as a miss.
10. **"Reuse the schematic and BOM."** Neither exists in the repo; only README
    §2 (ASCII wiring and a pin table).
11. `#ALERT miss sgv=65` carries the **last known** sgv, inherited from the
    existing `#ALERT` format. It is not a current value.
12. The cross-check was deliberately disabled (exit 2, "DISABLED") between
    `7cd58df` and this commit.
13. Docs vs firmware: `PROTOCOL.md` and the firmware were changed together,
    and no disagreement remains that I know of. The firmware stays
    authoritative.

## 5. Untested — written, never exercised

- **Firmware protocol 3 on hardware:**
  - whether `#REQ` reaches the phone;
  - `notifyEnabled()` gating;
  - `!TIME` handling;
  - the MISS buzz itself;
  - the clamp on alignment;
  - the added current.
- **The entire iOS app on a device:**
  - pairing;
  - state restoration and background relaunch;
  - the wake window;
  - `URLSession` against the live server (only `curl` has been tried);
  - chunked writes on a real stack;
  - `!GET STATUS` confirmation;
  - the UI;
  - the local notification;
  - reconnect pacing;
  - the 7-day expiry.
- **`GlucoseRing.xcodeproj`:** never opened or built.

## 6. Battery impact — calculated, not measured

**Old design:** a passive ring holding a live connection (200–400 ms
interval, slave latency 4) with a 1 s loop tick. README §11 estimates 50–150
µA idle; that is itself unmeasured.

**What the new cycle adds, every ~5 min.** At the ring's MTU of 23, a cycle
with a new reading is about 9 small packets:

| Direction | Packets |
|---|---|
| Ring → phone | `#REQ` 1, `#OK TIME` 1, `#STATUS` reply 3 |
| Phone → ring | `!TIME` 1, reading 2, `!GET STATUS` 1 |

That is roughly **5–9 extra connection events** the ring would otherwise skip
under slave latency. A cycle without a new reading is about 2–3 events.

**Cost:**

- At an assumed 5–10 µC per event (nRF52840 radio ≈ 5 mA at 0 dBm for
  1–2 ms, plus a CPU wake), that's 25–90 µC per 300 s, so **≈ 0.1–0.3 µA
  average**.
- Relative to the 50–150 µA baseline, that's **≤ 0.6 %**.
- On the 40 mAh cell at 100 µA, runtime goes from ≈ 400 h to ≈ 399 h.

**No new wake source** is needed: the timer check runs in the existing 1 s
loop.

**Alarms cost more than polling:** one MISS buzz is ≈ 85 mA × 2 s ≈ 0.17 C.
A full capped MISS episode (6 buzzes plus give-up) is ≈ 1 C, about 0.7 % of
the cell. This happens only when something is already wrong.

**Phone side:** about 288 background wakes a day, each with one HTTPS GET.
Unmeasured.

**To measure:** README checklist step 11 (µA meter inline), with pokes
answered, and with the phone away.

## Corrections — things I got wrong mid-task

- **Battery estimate.** Phase 0 said 2–4 extra events and +0.03–0.13 µA. I
  had not counted the `#OK TIME` reply, the `!GET STATUS` confirmation, or
  `#STATUS` spanning 3 notifications at MTU 23. Revised to 5–9 events and
  +0.1–0.3 µA; the README is corrected in this commit.
- **The firmware warning check.** My first grep could never match
  `glucose-ring.ino` (its pattern demanded a second file extension), so "no
  warnings" was unproven for the sketch. Rechecked with a working filter: the
  only warning is the intentional placeholder one.
- **A compile run showed "Exit code 1"** although both compiles returned 0.
  That was my shell's final `[ … ] && …` test, not a failure. Noted so the
  log isn't misread.
- **Reclaimed space.** In Phase 0 I estimated about 1.95 GB. It was 2.14 GiB
  by `df` and 1.92 GiB by `du`, and I did not explain the gap.
