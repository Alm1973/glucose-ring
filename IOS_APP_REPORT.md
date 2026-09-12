# iOS relay — report

**Status: STOPPED AT THE PHASE 0 GATE.** 2026-09-12. Nothing deleted,
uninstalled, installed, or changed. This file is the only new file.

---

## Phase 0 answers

### 1. Disk (`du -sh`, measured)

| Path | Size | Note |
|---|---|---|
| `/opt/homebrew/share/android-commandlinetools` (Android SDK) | **524M** | Homebrew cask `android-commandlinetools` |
| `~/.gradle` | **1.4G** | wrapper 8.11.1, AGP 8.7.3, Kotlin, org.json caches |
| `~/Library/Android` | **absent** | |
| `~/.android` | **3.5M** | adb keys/config, created when `adb` first ran |
| repo `android/` | **26M** | of which `app/build` 24M, `.gradle` 1.2M, `build` 132K (all gitignored) |

Related items that are **not** in your list; they may predate the Android
work, and I will not touch them unless asked:

| Path | Size |
|---|---|
| `/opt/homebrew/Cellar/gradle` (9.7.1) | 225M |
| `/opt/homebrew/Cellar/openjdk` (26) | 380M |
| `/Library/Java/JavaVirtualMachines/jdk-21.jdk` | 322M |
| `~/Library/Java/JavaVirtualMachines/openjdk-22.0.2` | 324M |

**Free disk: 9.9 GiB, volume 96 % full.** It was 12 GiB on 2026-09-11.

### 2. Reuse audit — what survives as design

No Kotlin survives. What survives is the list of rules that make the relay
safe, most of them now covered by tests I can port case for case.

| File | Verdict | What survives as design |
|---|---|---|
| `Relay.kt` | **Ports conceptually** | 15 min staleness limit; refuse future-dated readings (>2 min skew); refuse values outside 20–600; never fabricate; dedupe on `date`; "Relaying" only after the ring's own `#STATUS` confirms; the plain-language failure vocabulary (token rejected ≠ unreachable ≠ stale ≠ ring not connected); config/placeholder check. **Moves partly to firmware:** "polling stalled" becomes the ring's miss counter; the stale check becomes firmware-enforced with trusted time. **Mostly gone:** the 1–60 s backoff (see discrepancy 6). |
| `Nightscout.kt` | **Ports conceptually** | Strict parse: `sgv` whole number (refuse `"204"`, `204.5`); `date` epoch ms ≥ 1600000000000; `type` must be `sgv` if present; `direction` optional → empty. Caution: Swift `Codable` would happily coerce some of these, so it needs a hand-written decode. |
| `NightscoutClient.kt` | **Design ports, code gone** | `?token=`; hard timeouts (shorter in Swift, to fit the wake window); body size cap; **token-safe errors**. In Swift the risk is concrete: `URLError` carries the full failing URL, token included, in `userInfo`, so error objects must never be logged. |
| `NusClient.kt` | **Split** | *Ports:* the UUIDs; subscribe TX; trust the link only after `#BOOT`/`#VER`; "a write is not delivery", confirmed via `!GET STATUS`; chunking to the max write length; one outstanding write at a time; line reassembly. *Gone:* `createBond()` and the bond-state receiver (iOS has **no** app API to start pairing, see assumption 3); MTU requests (iOS negotiates itself); GATT `close()`/leak handling; binder-thread marshalling. |
| `RingProtocol.kt` | **Ports fully** | `formatReading`, `sanitizeDirection`, `parseRingLine`, command builders, `LineAssembler`, constants. New messages for the poke cycle are added here. |
| `RingService.kt` | **Platform code, gone** | Foreground service, wake lock, the 60 s `Handler` loop, `START_STICKY`, the persistent notification. The 60 s poll is inverted into the ring's poke. *Survives as principle:* status must be honest; send, then ask `!GET STATUS`. |
| `MainActivity.kt` | **Mostly gone** | Runtime permissions and the battery-exemption flow are gone. *Survives:* the scan filter (NUS service **and** name `GlucoseRing`), though iOS stores a peripheral **identifier**, not a MAC. Also the rule that a refused permission is reported, never a crash (iOS: `CBManagerAuthorization.denied`). |
| `BootReceiver.kt` | **Gone** | No iOS equivalent. State restoration replaces it, with gaps (assumption 5). |
| `RingProtocolTest.kt` | **Ports** | PROTOCOL.md §8 vectors. |
| `NightscoutTest.kt` | **Ports** | The live body fixture, every hostile case, every-truncation test. |
| `RelayTest.kt` | **Ports** | Staleness, dedupe, future-date, error-code cases, and the exhaustive "Relaying only when…" check. |
| `WireExportTest.kt` | **Ports** | Must be rewritten in Swift, because `tests/run_wire_crosscheck.sh` depends on it (discrepancy 9). The C++ harness `tests/wire_crosscheck.cpp` survives unchanged. |
| `build.gradle.kts` ×2, `settings.gradle.kts`, `gradle.properties`, wrapper jar/properties, `gradlew`, `gradlew.bat`, `strings.xml` | **Gone** | — |
| `AndroidManifest.xml` | **Gone** | Concepts map to `Info.plist`: `NSBluetoothAlwaysUsageDescription`, `UIBackgroundModes = bluetooth-central`. |

### 3. Safety net

- The working tree is **clean**. The current branch is `nightscout-relay`, tip `686093e`.
- `nightscout-relay` holds the **complete** Nightscout app: 22 tracked files
  under `android/`, including `Nightscout.kt`, `Relay.kt` and all 4 test files.
  `c306487` is the app commit; `686093e` added the cross-check harness and
  touched only a comment in `WireExportTest.kt`.
- **`main` was never merged.** It still points at `9712b21` and holds the
  **old xDrip+ app** (18 files, `XdripReceiver.kt`, no `Nightscout.kt`), plus
  the xDrip-era `PHONE_APP.md`.
- **Not in git** (gitignored, so `git rm` cannot restore them):
  `android/local.properties` (holds `sdk.dir`, the URL and **your pasted
  token**; it is no longer the placeholder), `android/app/build/`,
  `android/.gradle/`, `android/build/`.
- **There is no remote.** This `.git` directory is the only copy of the
  history. A deletion is recoverable only while this disk is intact.

Restore command, after the deletion:

```bash
cd ~/projects/glucose-ring && git restore --source=nightscout-relay -- android/
```

To restore exactly the app commit instead:

```bash
cd ~/projects/glucose-ring && git restore --source=c306487 -- android/
```

Either works as long as `nightscout-relay` (or `c306487` via `main`) stays
reachable. **Do not delete the `nightscout-relay` branch** unless `main` has
been fast-forwarded to it first.

### 4. iOS toolchain

| Item | State |
|---|---|
| macOS | 26.6.2 (25G83) |
| Xcode | **not installed** (no `/Applications/Xcode*.app`; Spotlight finds none) |
| Active developer dir | `/Library/Developer/CommandLineTools` (**Command Line Tools only**) |
| CLT version | **14.3.1** (2023): Swift **5.8.1**, no `xcodebuild`, no iOS SDK, no `simctl` |
| XCTest under CLT | **absent** (the CLT Frameworks dir holds only `Python3.framework`) |

**What building and running on a physical iPhone needs:**

1. **Xcode.** Command Line Tools cannot build an iOS app: no iOS SDK, no
   `xcodebuild`, no device signing. The Xcode version must support your
   iPhone's iOS version (which I don't know yet).
2. **You must install it yourself.** The Mac App Store and
   developer.apple.com both need your Apple ID sign-in, which I must not do.
   Accepting the license (`sudo xcodebuild -license accept`) and switching the
   active developer directory
   (`sudo xcode-select -s /Applications/Xcode.app`) need your password, so
   those are also yours to run.
3. **Disk: this is the blocker.** 9.9 GiB is free. Phase 1 reclaims about
   1.95 GB (SDK 524M + `~/.gradle` 1.4G + `~/.android` 3.5M +, if you allow,
   `android/` build outputs 25M), giving about 11.8 GiB. Xcode is a multi-GB
   download that expands to well over 10 GB. The download and the expanded
   app need space at the same time, and device-support components add more.
   **Plan for roughly 20–25 GB free before installing.** That's an
   *estimate*: Apple doesn't publish an exact installed size, and I have not
   downloaded it. You will need to free about 10 GB beyond what Phase 1
   reclaims.
4. **Signing.** Either a free Personal Team, where apps expire after
   **7 days** and must be re-installed from Xcode, or the paid Apple Developer
   Program ($99/yr), where builds last a year. For a relay you depend on, a
   7-day expiry is a recurring silent outage. The ring's miss alarm
   (Phase 2) would catch it, but that's the fallback, not the plan.
5. **On the iPhone:** Developer Mode on, trust this Mac, USB (or Wi-Fi after
   the first pairing).

**What can proceed without Xcode:** all of Phase 2 (firmware, using
`arduino-cli` + host C++ tests), and the pure Swift logic, which can be
compiled with CLT `swiftc` 5.8.1 and tested with a hand-rolled assertion
runner (no XCTest), the same style as `tests/test_all.cpp`. Only the app
target itself needs Xcode.

---

## 1. Done

- Audit only: git state, disk, the Android sources (file by file, from my own
  work in this session), toolchain, and the firmware loop and timing.
- Confirmed the four source-of-truth docs are unchanged since I last read or
  wrote them. The last commit touching any of them is `686093e`, mine.

## 2. Not done

Everything past Phase 0. It is blocked on your approval and on the decisions
below.

## 3. Assumptions

1. **Xcode's size**: "roughly 20–25 GB free" is an estimate, not a
   measurement.
2. **Free provisioning allows the `bluetooth-central` background mode.** From
   memory; not verified.
3. **iOS pairs with the ring on the ring's own Security Request.** iOS gives
   apps no way to start bonding. The ring's NUS characteristics are open
   (`SECMODE_OPEN`, README §6), so reading them won't trigger pairing either.
   The ring discards everything from an unencrypted link, so **if iOS ignores
   the ring's pairing request, the app never gets data.** This is the
   highest-risk iOS unknown. It can be checked early with nRF Connect on the
   iPhone as soon as a ring exists.
4. iOS wakes a suspended app for a notification on a subscribed
   characteristic of a connected peripheral, with about 10 s of execution
   time. From Apple documentation as remembered; not measured.
5. State restoration relaunches the app after a **system** kill, but **not
   after you force-quit it** from the app switcher, and, as I recall, not
   after a reboot until the app is opened again. Unverified. The ring's miss
   alarm is what catches both.
6. The battery figures in section 6 are calculated from generic nRF52840
   radio numbers, not measured.

## 4. Discrepancies

1. **"315 assertions." Actual: 343**, passing.
2. **"Committed on `nightscout-relay`, `c306487`."** True, but the tip is
   `686093e`, and **`main` was never merged**. `main` still carries the xDrip+
   app. A deletion branch cut from `main` would delete the *xDrip* version.
3. **"Add a request characteristic … Do not invent UUIDs."** These conflict.
   A new characteristic needs a new 128-bit UUID, which is inventing one. The
   ring already has a notify characteristic the phone subscribes to: NUS TX,
   `6E400003-…`. A poke can be a new `#` message on it (proposed: `#REQ`), so
   no new UUID is needed. iOS wakes the app for *any* notification on a
   subscribed characteristic.
4. **"Keep the existing reading characteristic for the write-back."** There
   is no separate reading characteristic. Readings and commands share NUS RX,
   `6E400002-…`, which remains the write-back channel.
5. **"RTC wake… the budget assumed the ring was passive."** The ring is not
   asleep between readings. `loop()` already runs every `TICK_MS` = 1000 ms
   (`glucose-ring.ino:905`), and it holds a live connection (200–400 ms
   interval, slave latency 4). The five-minute poke is a timer check in that
   loop; no new wake source is needed. The added current is only the extra
   BLE traffic (section 6).
6. **"Reconnect with backoff."** On iOS the idiomatic reconnect is a
   *non-expiring* `connect(peripheral)`, which the system services while the
   app is suspended. An app-level backoff timer can't even run while
   suspended. Backoff applies only to a connect-then-immediately-fail loop.
7. **"The ring must never act on a stale reading — use timestamps, not
   arrival time."** The ring has no clock of its own. Today it infers wall
   time *from the readings themselves*, the weakness recorded in
   PHONE_APP_REPORT.md discrepancy 6. Judging a reading's age from its wire
   timestamp needs the phone to also send its current time. That's a
   protocol change: proposed `!TIME <epoch_ms>`, protocol version 3.
8. **"Reuse the hardware design, schematic, and BOM."** No schematic or BOM
   file exists in the repo. The only hardware description is README §2 (ASCII
   wiring and a pin table).
9. **Deleting `android/` breaks `tests/run_wire_crosscheck.sh`**, which gets
   its bytes from the Kotlin `WireExportTest`. Until a Swift exporter exists,
   the runner should fail with a clear message, not a Gradle error.
10. **The miss alarm and the existing stale alarm coincide.** 3 misses × 5 min
    = 15 min, which is exactly the ring's existing stale window
    (`stale_minutes` 15) and its existing STALE pattern (2 × 60 ms, "no
    current data", already distinct from low/high). Proposal for Phase 2:
    - **MISS** means the phone did not answer at all: new, distinct pattern.
    - **STALE** means the phone answered, but the data is old.

    The app must then *answer every poke*, even with no fresh reading (for
    example with `!TIME` alone), so the ring can tell the two apart. Otherwise
    a broken app and a Dexcom outage buzz identically.
11. Your prompt says Nightscout pulls from Dexcom Share. That resolves
    PHONE_APP_REPORT.md assumption 11, which I had only inferred.

## 5. Untested

Nothing new written, so nothing new untested.

## 6. Battery impact — calculated, not measured

- **Baseline (unchanged):** a live BLE connection plus a 1 s loop tick,
  estimated in README §11 at 50–150 µA idle (itself unmeasured).
- **Added by the poke cycle, every 5 min:** one `#REQ` notify, plus
  receiving `!TIME` and the reading. That's about 3–5 packets at the default
  23-byte MTU (fewer at a larger one), so roughly 2–4 extra connection events
  the ring would otherwise skip under slave latency.
  - *Assumption:* about 5–10 µC per event (nRF52840 radio ≈ 5 mA at 0 dBm for
    1–2 ms including ramp-up, plus a CPU wake).
  - That's 10–40 µC per 5-min cycle, or **≈ 0.03–0.13 µA average**.
- **Relative cost: ≤ 0.3 % of the baseline.** On the 40 mAh cell at 100 µA,
  runtime goes from ≈ 400 h to ≈ 399.5 h. Buzzing (≈ 85 mA motor) costs far
  more, but only fires on alerts, including the new MISS alarm.
- **Phone side:** about 288 app wakes per day, each with one small HTTPS GET.
  Unmeasured.
- **To measure at bring-up:** README §9 step 11 (µA meter inline), with the
  poke enabled and disabled.

---

## Decisions needed

A. **Phase 1 branch plan.** Recommended: fast-forward `main` to
   `nightscout-relay`, so `main` carries the Nightscout app in its history,
   the current docs and the harness. Then cut `ios-relay` from `main` and
   `git rm -r android/` there.

B. **Uninstall scope.**
   - Android SDK + `~/.gradle`, as you asked.
   - Also `~/.android` (3.5M)?
   - Homebrew `gradle` + `openjdk` (605M): only if nothing else uses them.
   - JDKs: leave.

C. **Leftover ignored files in `android/`** after `git rm`:
   `local.properties` (**your live token**) and 25M of build outputs. Delete
   them after you've re-pasted the token into the iOS config? You told me not
   to `rm -rf` untracked files, so I'll only delete these specific paths if
   you say so.

D. **The poke channel.** `#REQ` on the existing TX characteristic
   (recommended: no new UUID), or a new characteristic with a UUID you choose.

E. **Also tell me:** your iPhone's iOS version, paid or free Apple developer
   account, and whether the ring hardware has arrived. Measuring sleep
   current needs it.
