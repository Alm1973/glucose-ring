# Phone app (Nightscout relay) — report

**Status: BUILT AND TESTED AS FAR AS POSSIBLE WITHOUT HARDWARE.** 2026-09-12.

The app is built (`app-debug.apk`, v0.2.0, minSdk 26 / targetSdk 34). **76 JVM
tests pass, 0 fail.** The firmware host suite is unchanged at **343 / 0**.
Nothing has run on a phone or against the ring.

**The APK that exists now has the placeholder token baked in**, so on a phone
it would show *NOT relaying: not configured*. Paste the token, rebuild, then
install (see "Not done").

Nothing is committed. Changes are in the working tree.

---

## 1. Done — and how each was verified

### Phase 0 (gate)
The toolchain turned out to be already installed, and an xDrip+-based app
already existed. I reported both and stopped. You chose: reuse the app and
replace its input with Nightscout; bake the token into the APK; approve
`org.json` as a test dependency; stay on API 34.

### Phase 1 — toolchain
- **Installed:** only `org.json:json:20240303`, fetched by Gradle into
  `~/.gradle/caches/modules-2/files-2.1/org.json/`, **test classpath only**.
  It is not in the APK (`unzip -l` shows 0 `org/json` entries; the phone uses
  Android's built-in copy).
  - *Undo:* delete the `testImplementation("org.json:json:…")` line in
    `android/app/build.gradle.kts`, then
    `rm -rf ~/.gradle/caches/modules-2/files-2.1/org.json`.
- **Nothing else installed.** Before changing any code I verified the existing
  toolchain with an **offline** build of the old app: `BUILD SUCCESSFUL`,
  22 tests / 0 failures.

### Phase 2 — protocol, read not invented
- **UUIDs match**: PROTOCOL.md = `NusClient.kt` = the byte arrays in the
  compiled-in core's `BLEUart.cpp:45-60` (the sketch itself declares none).
  Service `6E400001-…`, RX `6E400002-…` (write, with or without response),
  TX `6E400003-…` (notify).
- **Wire format confirmed against `reading_parser.cpp` and `pumpUart()`**:
  `<sgv>,<direction>,<epoch_ms>\n`. It is `\n`-terminated with **no NUL on the
  wire**; `\r` is ignored; the line is capped at 95 bytes.
- **Live endpoint confirmed** (you ran the curl): HTTP 200, `sgv` integer,
  `date` 13-digit ms, `direction` string, `type` sgv. It matches PROTOCOL.md's
  assumptions, so there is **no finding**. That exact body is now a test
  fixture.

### Phase 3 — the app
- **Nightscout poll every 60 s** (`RingService` + `NightscoutClient`), token as
  `?token=`, read-only, 15 s timeouts, 64 KB body cap.
- **Strict parsing** (`Nightscout.kt`): wrong types are refused, never coerced.
- **Relay rules** (`Relay.kt`, pure):
  - Only readings **≤ 15 min old** are sent.
  - Dedupe on `date`, per link.
  - Future-dated readings and values outside 20–600 are refused.
  - Nothing is ever fabricated.
- **Plain-language notification**: *Relaying* appears **only** when a fresh
  reading was written **and the ring's own `#STATUS` confirms it**. Every
  other state says *NOT relaying: <reason>*. That covers all the cases you
  listed: Nightscout unreachable, 401/403, reading older than ~15 min, ring not
  connected. It also covers permission refused, Bluetooth off, no ring chosen,
  not paired, ring not answering, empty/garbled/future data, and polling
  stalled.
- **BLE** (`NusClient.kt`), in order:
  1. connect
  2. **bond**
  3. MTU
  4. discover
  5. subscribe
  6. `!VER` handshake: the link counts as up only after `#BOOT`/`#VER`
  7. queued, MTU-chunked writes
  8. `!GET STATUS` after each reading

  Reconnect uses 1 s → 60 s backoff with jitter, reset only after the ring
  answers.
- **Foreground service** (`connectedDevice`), partial wake lock, `START_STICKY`,
  boot restart, battery-exemption button.
- **Permissions**: `BLUETOOTH_CONNECT`/`SCAN` requested at runtime. Refusal
  never crashes: every BLE call is behind a check, and the status says "no
  Bluetooth permission".
- **"Find ring"**: scans for the NUS service **and** the name `GlucoseRing`.
- Removed `XdripReceiver.kt` and `Reading.kt` (in git history). Updated
  `PHONE_APP.md` and the README layout.

### Verification (none of this is "it compiles")
| Check | Result |
|---|---|
| JVM unit tests | **76 run, 0 failures, 0 errors**: NightscoutTest 22, RelayTest 32, RingProtocolTest 21, WireExportTest 1 |
| Firmware host suite | **passed: 343 failed: 0** (unchanged) |
| App output through the **firmware's real `rp_parse()`** | `WireExportTest` dumped 27,888 wire lines (all sgv 20–600 × 16 directions incl. hostile ones × 3 epochs), **829,314 bytes**. A host harness compiled against `reading_parser.cpp` fed them in **20-byte chunks** through a copy of `pumpUart()`'s framing: **27,888 decoded exactly, 0 bad, 0 unterminated bytes.** |
| Live body → wire line | `NightscoutTest`: the real response gives `204,FortyFiveDown,1789184103083\n` |
| Exhaustive "Relaying" check | `RelayTest`: 9 Nightscout outcomes × 8 link states × 3 ring replies; "Relaying" appears for exactly the one right combination |
| Every truncation of the live body | rejected (`NightscoutTest`) |
| APK metadata (`aapt2 dump badging`) | sdk 26 / target 34, expected permission set |
| Token not leaked | `BuildConfig` checked by `grep -c` only, never printed. No `Log` call references the token, URL or `BuildConfig`. The one fetch log prints a `Fetch` value (HTTP code or fixed text). `NightscoutClient` maps all exceptions to fixed text. |

The harness is in this session's scratchpad (`wire_check.cpp`) and is **not
committed**. Say if you want it in `tests/`.

## 2. Not done

| Item | Blocked on |
|---|---|
| Real token in the APK | You pasting it into `android/local.properties` (`nightscout.token=`), then rebuilding |
| Install on a phone | No phone attached (`adb devices` was empty) |
| Anything with the ring | Hardware not arrived |
| Commit | Your go-ahead |
| Follow the ring's configured stale window | The app uses a fixed 15 min (ring default). If you change it with `!SET STALE`, the app won't follow; reading `!GET STATUS`/`!GET STALE` on connect would fix it. Not built. |
| Surface `#ALERT stale_giveup` | Not built. The notification already says *NOT relaying* in that situation, but not that the ring stopped buzzing. |
| Config UI (PHONE_APP.md §9) | Out of scope for this task |
| `REPORT.md` still describes the xDrip+ design | Left alone as the previous session's record. `PHONE_APP.md` is now the current spec. |

**Next steps, in order:**
1. Replace `PASTE_READ_ONLY_TOKEN_HERE` in `android/local.properties`.
2. Rebuild:
   ```bash
   cd ~/projects/glucose-ring/android && ANDROID_HOME=/opt/homebrew/share/android-commandlinetools ./gradlew :app:testDebugUnitTest :app:assembleDebug
   ```
3. Enable USB debugging on the phone, plug it in, then install:
   ```bash
   /opt/homebrew/share/android-commandlinetools/platform-tools/adb install -r ~/projects/glucose-ring/android/app/build/outputs/apk/debug/app-debug.apk
   ```
4. **Worth doing before the ring arrives:** open the app and grant permissions.
   The notification should read *NOT relaying: no ring chosen* followed by
   *Reading 204 …, N min old*. That proves the whole Nightscout path on a real
   phone. Then turn on airplane mode, expect *can't reach Nightscout*, and
   leave it overnight with the screen off.

## 3. Assumptions — inferred, not read

1. **Thresholds are mine**: stale = 15 min (the ring's default), future
   tolerance = 2 min, "polling stalled" = 5 min, handshake = 3 × 10 s, pairing
   wait = 60 s.
2. `sgv` written as `204.0` is accepted as 204; `204.5` and `"204"` are
   refused. A missing/`null` direction is sent as empty. Only the single entry
   I saw is real data.
3. **`CHANGE_NETWORK_STATE` satisfies API 34's `connectedDevice` prerequisite**
   so a refused Bluetooth permission can't crash `startForeground()`. This is
   from memory of the Android docs, not verified on a device.
4. **A foreground-service process keeps network access in Doze.** From memory,
   unverified.
5. **A permanent partial wake lock is needed for a 60 s poll with the screen
   off, and is enough.** Unverified; battery cost unmeasured.
6. On API 33+ the framework still calls the deprecated 2-argument
   `onCharacteristicChanged` when the 3-argument one isn't overridden. From
   memory of AOSP. If wrong, the ring's replies never arrive and the app sits
   at *ring not answering*: a visible failure, not a silent one.
7. `ACTION_BOND_STATE_CHANGED` reaches a runtime receiver registered
   `RECEIVER_EXPORTED` (it is a protected broadcast).
8. Android's low-latency scan is active, so it receives the ring's scan
   response, where the name lives (`glucose-ring.ino:856`).
9. Android's built-in org.json behaves like `org.json:20240303` for these
   inputs. The tests run against the latter.
10. The phone's clock is roughly right. Staleness compares phone time with
    Nightscout's `date`. A wrong clock shows up as stale or "dated in the
    future", which is visible, not silent.
11. `nightscout-connect` means Nightscout pulls from Dexcom Share. That's
    inferred from the device name only.

## 4. Discrepancies

1. **Prompt: "toolchain not installed." Actual: installed** (previous session).
2. **Prompt describes a new app. Actual: an xDrip+ app existed.** Resolved by
   your decision A (reuse).
3. **Prompt: 315 host assertions. Actual: 343**, and still 343 / 0.
4. **Prompt: one "characteristic UUID". Actual: two, plus bonding.** The ring
   silently discards bytes until encrypted and never acks readings, so the app
   treats the link as up only after the ring answers.
5. **Prompt wants staleness surfaced; PHONE_APP.md §7 forbids app-side
   alerting.** Resolved as silent notification text, never sound or vibration.
6. **The firmware cannot catch a stale first reading after it reboots**
   (`glucose-ring.ino:632-636, 706-718`). The app's "never send stale" rule is
   the only guard for that case. This is a firmware limitation, recorded here
   and not fixed.
7. **The old app marked readings "sent" before bonding and couldn't pick a
   device.** Both fixed.
8. **Prompt: "a few hundred lines, not a framework." Actual: 1,180 lines of
   main source** (99 of them the unchanged `RingProtocol.kt`) and 549 lines of
   tests. Most of it is the bond → handshake → `!GET STATUS` confirmation path
   in `NusClient`, and the status wording in `Relay`. Both exist so the app
   can't look healthy while relaying nothing. It is over your stated size.
   There are no frameworks or third-party runtime dependencies, but it could
   be trimmed if you'd trade some of that.
9. PHONE_APP.md §7 suggested `autoConnect = true` for idle periods. I used one
   mechanism (`autoConnect = false` + app backoff), because the old code
   combined both and each cancelled the other. PHONE_APP.md now says so.
10. The APK declares `com.glucosering.ring.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION`.
    I did not add it: androidx.core's `ContextCompat.registerReceiver` does.
    It is harmless (own package, signature-level).

## 5. Untested — written but not exercised

**Everything that runs on a phone.** None of the following has executed:
- **Bluetooth:** the GATT flow, bonding and the pairing dialog, the handshake,
  chunked writes on a real stack at MTU 23, and the `!GET STATUS`
  confirmation.
- **Link recovery:** reconnect and backoff, and "Find ring" scanning.
- **Staying alive:** foreground-service survival with the screen off, in
  Doze, and under OEM task killers; restart after boot; wake-lock battery
  cost.
- **User-facing paths:** permission-refusal handling on a device, and how the
  notification renders.
- **Nightscout from the app:** `NightscoutClient` itself, including TLS and
  the 401 path. The live endpoint was verified with curl, not with this code.

Only the pure layers (`Nightscout.kt`, `Relay.kt`, `RingProtocol.kt`) are
tested, plus the byte-level check against the firmware parser.

## Corrections — things I got wrong mid-task

- Before running the cross-check I said it would be **26,112** lines. It was
  **27,888**: I had counted 15 test directions instead of 16. The harness
  compares against the exported expectation file, so the check itself was not
  affected.
- In Phase 0 I called the old app's **22 JVM tests** reusable. One of them
  tested `ReadingGate`, which I removed along with that class, so 21 remain.
  The dedupe behaviour it covered is now tested in `RelayTest`.
