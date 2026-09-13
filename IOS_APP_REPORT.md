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
| Deployment target | **Resolved:** iPhone 11 on iOS 26.6.2; set to iOS 26.0 (session (b), §7). |
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

---

## Session 2026-09-12 (b): backup, disk, cleanup before Xcode

**Status: DONE.** The repo is backed up to GitHub and the cleanup is done.
Sections 1–5 are the read-only analysis as presented to you; the measured
results after your go-ahead are in section 6.

### 1. Backup — blocked on a GitHub credential

**Repo:** `github.com/Alm1973/glucose-ring`. It is **not public**: GitHub's
unauthenticated API returns 404, where a public repo returns 200. I can't
check whether it is empty until I can authenticate.

**Safe to push:**
- The 9 commits reachable from any ref never contained a file named
  `Secrets.plist` or `local.properties`, on any branch, at any point in
  history.
- The live token's *value* appears in **0** locations across every commit.
  It was searched without being printed.
- Both files are still ignored (`.gitignore:22`, `.gitignore:28`).

**Local repo:** clean; branches `main` and `nightscout-relay` at `686093e`,
`ios-relay` at `7aa9927`; no tags, no stash; `.git` is 1.0 MB.

**Why I can't push yet:**
- `gh` is not installed.
- There is no HTTPS credential helper and no Keychain entry.
- `~/.ssh/id_ed25519` exists, but **GitHub lists 0 keys for Alm1973**, and
  SSH is refused. The key is also passphrase-protected and not loaded in
  `ssh-agent`.
- GitHub's host key was verified against the fingerprint GitHub publishes
  over TLS (`SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU`). It is held
  in a scratch file only; `~/.ssh` is untouched.

**Planned once you've added the key and approved:**
1. Add the verified GitHub host key to `~/.ssh/known_hosts`.
2. `git remote add origin git@github.com:Alm1973/glucose-ring.git`.
3. `git ls-remote origin`. Stop if the repo is not empty.
4. `git push origin main nightscout-relay ios-relay`.
5. Verify with `git ls-remote --heads origin`: all three branches, with SHAs
   matching the local ones.

### 2. Disk — measured, nothing deleted

**Totals:** APFS container 245.1 GB; **free 12.1 GB** (`df`: 11 GiB avail);
Data volume 192 GiB used.

**Safe to delete** (regenerable caches; quit the app first). Subtotal
**≈ 12 GB**.

| Path | Size | What breaks |
|---|---|---|
| `~/Library/Caches` (all) | 9.3 GB | Apps rebuild their caches; slower first launch. Inside: Google 2.6G, Spotify 1.7G, Brave 1.5G, Google SoftwareUpdate 0.67G, Homebrew 0.65G, JetBrains 0.47G (IntelliJ is no longer installed), ms-playwright 0.45G (re-downloads its browsers), WhatsApp 0.43G, Zoo updater 0.3G, pip 0.13G |
| `~/Library/Application Support/discord/Cache` | 756 MB | Discord re-downloads media |
| `~/.cache/huggingface` | 680 MB | Models re-download on next use |
| `~/Library/Application Support/Spotify/PersistentCache` | 625 MB | Streaming cache re-buffers |
| Claude `Cache` + `Code Cache` | 451 MB | App caches rebuild |
| `~/Library/Logs` | 201 MB | Old logs only |
| `brew cleanup` | ~229 MB | Old Homebrew versions; overlaps the Homebrew cache above |
| `~/.npm` | 25 MB | npm cache |

**Ask first** (not caches, or not regenerable), ranked by size:

| Path | Size | What breaks |
|---|---|---|
| `~/.ollama/models` | 11 GB | 4 local models (llava 4.7, qwen2.5vl 3.2, llama3.2 2.0, moondream 1.7 GB); gone until `ollama pull` again |
| `~/Library/Application Support/Claude/vm_bundles` | 9.8 GB | Claude desktop's VM image for its sandboxed features. The app re-downloads it. **This session runs inside that app**: quit it first. |
| `/Users/Shared/Epic Games/Fortnite` | 9.4 GB | Fortnite install. The Epic Games Launcher isn't installed, so it's likely orphaned. Re-download is about the same size. |
| `~/Downloads`: 98 videos | 7.3 GB | Your own videos (`IMG_*.MOV`), gone unless kept elsewhere |
| `~/Downloads`: 77 installers & archives | 5.9 GB | `.dmg`/`.pkg`/`.zip`; installed apps are unaffected |
| `/Applications/iMovie.app` | 2.5 GB | Reinstallable from the App Store |
| Chrome profiles 4, 3, 8 | 3.2 GB | Browsing data; clear the cache from inside Chrome instead |
| `~/Downloads`: images | 1.7 GB | Personal |
| Google `DriveFS` | 1.2 GB | Google Drive's local cache; **may hold unsynced files** |
| `/Applications/ParaView-5.13.3.app` | 1.2 GB | Reinstallable |
| `~/Movies` | 1.1 GB | Personal |
| `~/Downloads/jdk-21.0.4.jdk` (×3) + `jdk-21_macos-aarch64_bin.tar.gz` | 1.1 GB | Copies of the JDK already installed in `/Library/Java`. *Assumed* unused. |
| Google `GoogleUpdater` | 759 MB | Chrome's updater rebuilds it |
| `~/.p2` | 656 MB | Eclipse bundle pool. No `Eclipse.app` was found, so it's *likely* leftover. |
| `~/Library/Roblox` | 576 MB | Roblox data |
| `~/mlx_env`, `~/robotenv`, `~/tf_env` | 0.49 / 0.20 / 0.01 GB | Python venvs; projects using them break until recreated |

**Keep:**
- `~/Library/Arduino15` (1.6 GB): the firmware build needs the Seeeduino core.
- `/Library/Developer/CommandLineTools` (2.6 GB): Homebrew, `arduino-cli`,
  `clang` and `swiftc` use it, at least until Xcode is installed.

**Could not measure.** About 90 GB of the used space is hidden from this
shell by macOS privacy protection:
- `~/.Trash`, Mail, Messages, Safari, and iOS device backups (MobileSync);
- `~/Pictures`, which reads 0 B here and is probably hidden too;
- the two other accounts, `s_khidake` and `sakshiskhidake`;
- system data.

**System Settings → General → Storage** sees all of it. Check the Trash and
iOS backups there first; they are often the biggest easy wins.

**Getting to ~30 GB:** safe caches (≈ 12) + Ollama (11) + the Claude VM (9.8)
≈ 33 GB. Alternatively, Fortnite (9.4) + Downloads installers and the JDK
copies (7.0) + caches (12) ≈ 28 GB.

### 3. Android leftovers — checked, deletion pending

**Measured, ready to delete:** `android/` is 26,268 KiB (696 files, 0
tracked):

| Path | KiB |
|---|---|
| `local.properties` | 4 |
| `app/build` | 24,940 |
| `.gradle` | 1,192 |
| `build` | 132 |
| `.kotlin` | 0 |

**`local.properties` still holds the only local copy of your token.**
`Secrets.plist` still has the placeholder, so deleting it now would lose the
token.

**Homebrew `gradle` 9.7.1 (225M) and `openjdk` 26.0.2.1 (380M):**
- `brew uses --installed`: nothing depends on `gradle`; `openjdk` is used
  only by `gradle`.
- No other Gradle or Maven project exists under `~`.
- No shell config refers to them.
- They are not linked into `/Library/Java`.

**Safe to remove.**

**JDKs: leaving them.** I can't show that nothing needs them.
- `jdk-21` (Oracle package, installed 2024-08-06) and `openjdk-22.0.2`
  (downloaded by IntelliJ, 2024-08-02; it is the default `java`).
- The OpenFOAM (SimFlow) app bundles no Java of its own at depth ≤ 5, so it
  may use one of these. Eclipse leftovers (`~/.p2`, `~/eclipse-workspace`)
  also remain.

### 4. Contradictions with the previous report

1. **Homebrew `gradle` and `openjdk` did not predate the Android work.** They
   were installed on **2026-09-10 at 11:21**, three minutes after the Android
   SDK cask (11:18). `gradle` was installed on request and `openjdk` pulled in
   as its dependency, i.e. by the earlier session. `PHONE_APP.md`,
   `REPORT.md` and §1 above described them as already present ("may predate
   the Android work"). That was wrong.
2. **Free space:** the report said 11.81 GiB. Now `df` shows 11 GiB and the
   container 12.1 GB (≈ 11.3 GiB); it moves with other activity.
3. **A mistake in my own check this session:** my first HTTPS probe of the
   repo used `timeout`, which macOS doesn't have, so it never ran. It is
   superseded by the SSH checks above.
4. **Not a contradiction, but worth knowing:** the Nightscout URL (not the
   token) is in committed docs. Keep the repo private.

### 5. iOS deployment target — what I need

- Your iPhone's **iOS version** (Settings → General → About → iOS Version)
  and **model name** (same screen).
- Once installed, the **Xcode version**. It must support that iOS version.

The deployment target is a *minimum*: the code typechecks at iOS 16.0 and
needs nothing from 17. So if your phone is on iOS 16, I'll set 16.0;
otherwise 17.0 already works, or I can raise it to match.

### 6. Results after your go-ahead (measured)

**Backup: done.**
- Remote `origin` is `git@github.com:Alm1973/glucose-ring.git`, over SSH,
  authenticated as Alm1973. The repo was confirmed empty (`git ls-remote`)
  before pushing.
- Pushed `main`, `nightscout-relay` and `ios-relay`; each now tracks
  `origin`.
- **Verified:**
  - `git ls-remote --heads origin` shows `ios-relay` at `7aa9927`, and `main`
    and `nightscout-relay` at `686093e`. All match the local branches.
  - A fresh `--mirror` clone of the remote holds **all 9 commits** (9
    locally).
  - **0 files named `Secrets.plist` or `local.properties`** anywhere in the
    remote's history.
  - The anonymous API still returns 404, so the repo is not public.
- GitHub's host key was re-verified against `api.github.com/meta` and was
  already in `~/.ssh/known_hosts`, so I did not modify it.
- This report update is **not yet committed or pushed**.

**Token:** moved file-to-file from `android/local.properties` into
`ios/GlucoseRing/Secrets.plist` by a script that never printed it.
- It read back identical (21 characters), and the URL matched.
- The plist lints, is still gitignored, and git lists it nowhere.

**Reclaimed, measured with `df`:** free space went from 11,425,316 KiB
(10.90 GiB) before to **24,473,956 KiB (23.34 GiB)** after. A few minutes
later it read 23.22 GiB, as Chrome rebuilt its cache.

| Step | Freed (KiB) | Notes |
|---|---:|---|
| `android/` leftovers | 25,620 | Includes `local.properties` and the old build's baked `BuildConfig` and APK, both of which contained the token |
| Homebrew `gradle` + `openjdk` | 888,620 | Plus 27 orphaned dependencies Homebrew removed automatically (correction 1) |
| Safe caches | 12,135,340 | `~/Library/Caches` except Claude/Anthropic entries (9.74 GB); Discord cache; Spotify stream cache; Hugging Face cache; `~/Library/Logs`; npm cache; `brew cleanup` (70 MB). macOS refused 12 protected cache folders, which remain. |
| **Total freed now** | **13,048,640 (12.44 GiB)** | |
| Downloads installers & archives, moved to the Trash | 0 until emptied | 69 files, 4,370,272 KiB (4.17 GiB): 31 zip, 22 dmg, 14 pkg, 2 gz. **Empty the Trash to free them.** |

**Not done:**
- **The Claude app's VM files (9.8 GB) and Claude's caches (451 MB).** They
  are open in the running Claude app (4 and 2 open files), which this session
  runs in. Deleting them would free nothing while they are open, and could
  break the running VM. To remove them, quit Claude and then run:
  ```bash
  rm -rf ~/Library/Application\ Support/Claude/vm_bundles
  ```
  The app re-downloads the VM when it next needs it, so that space may come
  back.
- **The three JDK copies:** already gone before my run (correction 2).
- **Left alone, per your instruction:** the Ollama models (11 GB) and
  Fortnite (9.4 GB). The JDKs are untouched too.

**Projection:** emptying the Trash (+4.17 GiB) and removing the Claude VM
(+~9.1 GiB) would bring free space to about **36–37 GiB**. That clears the
20–25 GB Xcode estimate, although the total freed (~26 GiB) is short of your
30 GB target.

**Still works:**
- host tests 450/0;
- iOS core tests 112/0;
- wire cross-check 27,888/27,888;
- both JDKs present;
- the Seeeduino core installed;
- `brew missing` clean;
- `arduino-cli`, `cmake`, `python3.14`, `llama.cpp`, `pkgconf` and `swiftc`
  all run.

**Where I drew the line:** I don't permanently delete your own files, even
with approval, so the Downloads items went to the Trash for you to empty.
Regenerable things (caches, build output, Homebrew packages) I removed
directly.

**Corrections and contradictions from this step:**
1. **Homebrew removed more than I told you.** By default, `brew uninstall
   gradle openjdk` also removes dependencies nothing else needs, and it took
   **27 more packages** (≈ 240 MB): `little-cms2`, `harfbuzz`,
   `gradle-completion`, `libtiff`, `graphite2`, `cairo`, `icu4c@78`,
   `pixman`, `glib`, `webp`, `fontconfig`, `libxrender`, `libxext`, `lzo`,
   `freetype`, `giflib`, `gettext`, `pcre2`, `libx11`, `jpeg-turbo`,
   `json-c`, `libpng`, `libunistring`, `libxcb`, `libxau`, `libxdmcp`,
   `xorgproto`. I didn't warn you. `brew missing` reports nothing broken, and
   any of them can come back with `brew install <name>`.
2. **The three extracted JDK copies and `jdk-21_macos-aarch64_bin.tar.gz`
   were no longer in Downloads when the cleanup ran; my run did not move
   them.** Downloads is now 7.69 GiB. It was 16.18 GiB at the analysis, and
   I moved 4.17 GiB, which would leave ~12.0. So about 4.3 GiB left Downloads
   in between, by something other than my run: those JDK items plus 8 of the
   77 archives I had counted. If you trashed them, emptying the Trash frees
   them as well. I can't check: the Trash can't be listed from here, and
   even a file I had just moved there doesn't show up.
3. **Free space before the cleanup was 10.90 GiB,** a little under the
   11 GiB / 12.1 GB measured an hour earlier. Normal drift.

### 7. Deployment target: set

**Your phone:** iPhone 11, iOS 26.6.2.

**Change:** `IPHONEOS_DEPLOYMENT_TARGET` is now **26.0** in
`ios/GlucoseRing.xcodeproj`, in both configurations. It was 17.0.

**Why 26.0, when 17.0 would also install?** The deployment target is only a
minimum, and the code needs nothing newer than iOS 16. But background
Bluetooth wake, state restoration and pairing are exactly the behaviours that
change between iOS releases, and they will only ever be tested on iOS 26.
Declaring 26.0 makes the versions the app claims to support match the version
it is tested on.

**Knock-on change:** SwiftUI's `.foregroundColor` (deprecated since iOS 17)
is replaced by `.foregroundStyle` (available since iOS 15), in 3 places in
`GlucoseRingApp.swift`. At a 26.0 target the old call would have produced
deprecation warnings. No behaviour changes.

**Xcode:** you need **Xcode 26**, for its iOS 26 SDK. It fetches device
support for 26.6.2 the first time the phone is connected. macOS 26.6.2 runs
Xcode 26.

**What I can't verify here:** the Command Line Tools SDK is macOS 13.3, so
my Catalyst typecheck cannot target iOS 26. I re-ran it at iOS 16.0 after
this edit. Raising the minimum removes no API, but newer deprecations will
only show up in Xcode itself.

### 8. Xcode: found already expanded, now installed

**The download:** `~/Downloads/Xcode_27_Release_Candidate.xip`,
2,014,229,334 bytes. It is **complete** (its archive table matches the file
size) and **signed "Apple Software"**.

**It had already been expanded.** `~/Downloads/Xcode.app` was **Xcode 27.0,
build 27A266a**, 3.6 GB, and its full code signature was valid
(`codesign --verify --deep --strict`). Nothing needed unzipping.

**Installed.** I moved it (a same-disk rename) to `/Applications/Xcode.app`.
- The signature re-verified in place.
- `xcodebuild -version` runs through a `DEVELOPER_DIR` override, without
  changing any system setting.
- It needs macOS 26.6 or later (this Mac runs 26.6.2).
- It ships the iOS 27 SDK and builds for iOS 26.0 and later, including your
  iPhone 11 on 26.6.2.

**Still yours** (each needs your admin password):
1. Accept the license: open Xcode and agree, or run
   `sudo xcodebuild -license accept`.
2. Let it install its first-launch components:
   `sudo xcodebuild -runFirstLaunch`.
3. Optional: make it the system default with
   `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`. The
   default is still Command Line Tools, so the firmware build and both test
   scripts keep running exactly as tested.

**Next for me, once the license is accepted:** the first real iOS compile of
this app, unsigned:
`xcodebuild -target GlucoseRing -sdk iphoneos CODE_SIGNING_ALLOWED=NO build`.

The `.xip` (2 GB) is now redundant; move it to the Trash when you're happy
with Xcode.

**Corrections:**
1. **Xcode's size.** I estimated 20–25 GB installed. Xcode 27 is 3.6 GB
   expanded, from a 2.0 GB download. The cleanup still helped, but Xcode
   didn't need it.
2. **Xcode 26 vs 27.** I wrote that you needed Xcode 26. You installed the
   27.0 release candidate, which is newer and fine.
3. **My first completeness check of the `.xip` was wrong.** A regex paired
   the Metadata entry's offset with the Content entry's length, and reported
   the download as truncated at 50 %. Re-parsed with an XML parser, it is
   complete.
4. **A misleading line in my own output.** After the move, `xcode-select -p`
   printed the Xcode path only because my shell had `DEVELOPER_DIR` set. The
   real system setting is unchanged; I checked it separately without the
   override.

### 9. First iOS build: succeeds

**How it was built:** Xcode 27.0 (iOS 27 SDK, Swift 6.4), unsigned, Debug,
for `iphoneos`. I used a `DEVELOPER_DIR` override, so the system default
stayed Command Line Tools. The output went to a scratch folder, not the repo.

**Result:**
- **BUILD SUCCEEDED, with 0 errors and 0 compiler warnings.** The one warning
  in the log comes from App Intents metadata extraction ("no
  AppIntents.framework dependency found"); the app doesn't use App Intents.
- **The hand-written Xcode project works.** All 8 source files compiled: 5
  from the app folder and 3 from `Core`, through the synchronized groups.
- **The built app is 504 KB:**
  - bundle ID `com.glucosering.relay`;
  - `MinimumOSVersion` 26.0;
  - `UIBackgroundModes` `[bluetooth-central]`;
  - `NSBluetoothAlwaysUsageDescription` present;
  - iPhone only.
- **Binary:** Mach-O arm64, platform iOS, minos 26.0, SDK 27.0.
- **`Secrets.plist` is bundled with your real token,** checked by count and
  never printed. As noted before, the token is inside the built app.
- **The Swift core tests pass under Xcode's Swift 6.4,** warnings as errors:
  112/0, as under Swift 5.8.
- My worry that the newer compiler would raise concurrency errors did not
  materialise; the project builds in Swift 5 language mode.

**Not done** (needs you and the phone):
- signing with your Personal Team;
- installing and running it on the iPhone.

No simulator runtime is installed.

### 10. Runs on the iPhone

**Installed** by you from Xcode, onto your iPhone 11 on iOS 26.6.2.
`devicectl` confirms **Glucose Ring 0.1.0 (build 1), `com.glucosering.relay`**
in the device's app list.

**First launch shows exactly the expected no-ring state:**
- "Waiting for the ring", in red, because no data flows until a ring
  pokes;
- "No ring chosen yet. Tap Find ring."

That second line also tells us something:
- CoreBluetooth is **powered on and authorised**; a refusal would show
  "Bluetooth permission was refused" instead.
- The app launched through the SwiftUI and AppDelegate path, and created its
  central manager without crashing.

**Signing:**
- Personal Team `TUQ2656V4Y`; Xcode added `DEVELOPMENT_TEAM` to both target
  configurations, and that change is committed.
- The profile "iOS Team Provisioning Profile: com.glucosering.relay" was
  created 2026-09-13 04:26 UTC, covers 1 device, and **expires 2026-09-20
  04:26 UTC, which is Saturday 2026-09-19 at 21:26 MST.**
- After that the app will not launch until it is reinstalled (connect the
  phone, then press Run in Xcode). While it's not running, the ring buzzes
  MISS.
- My first script put the expiry at "7 days 6 h" away. It compared a UTC
  date with local time and was off by the 7-hour offset; the dates above
  are corrected.

**Still untested** (all of it needs the ring):
- pairing;
- `#REQ` wakes, including background and relaunch after the system kills
  the app;
- the Nightscout fetch from the app. The app only fetches when the ring asks,
  so that path has not run on the phone yet.
- the writes, and `!GET STATUS` confirmation;
- the "token rejected" notification.

Also untested: the firmware on the XIAO itself.

### 11. Cosmetics: app icon and the name "GRing"

**App icon:** a brushed-silver ring with a red glucose drop, on a
navy-to-teal background.
- It is drawn by `ios/tools/make_app_icon.swift` (Core Graphics; it runs
  with Command Line Tools). The output is
  `ios/GlucoseRing/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png`:
  1024×1024 and opaque (`sips`: `hasAlpha: no`).
- Xcode is told to use it via `ASSETCATALOG_COMPILER_APPICON_NAME = AppIcon`
  in both target configurations.
- The unsigned build includes the compiled icon (`AppIcon60x60@2x.png`,
  `Assets.car`), and `Info.plist` names it (`CFBundleIconName`). 0 errors,
  0 warnings.

**Name shown on the phone: "GRing".** Changed in three places:
- `CFBundleDisplayName`;
- the Bluetooth permission text;
- the "token rejected" notification title.

**Unchanged, on purpose:**
- the Xcode target, project and bundle ID `com.glucosering.relay`. A new
  bundle ID would be a different app to iOS and would redo signing.
- the ring's Bluetooth name `GlucoseRing`, which the firmware advertises and
  the app scans for.
