# glucose-ring — work report

**Version control is no longer the top risk.** You were right that it was the
biggest live risk and wasn't in the risk sections. It is now resolved:
everything is committed in a standalone repo at `~/projects/glucose-ring`
(`4297cf0`, `c612140`, `d1e58ae`) and `~/.git` is gone.

Hardware has not arrived. Firmware is verified by **compiling** and by
**host-side tests**; the phone app by **compiling** and **JVM unit tests**.
Nothing was uploaded to a board or installed on a phone.

---

## DONE

### 1. Commit — done first

`4297cf0` put the entire v2 firmware under version control before anything
else was touched. Two further commits followed (`c612140`, `d1e58ae`).

### 2. Home-directory git repo — fixed

- New standalone repo at `~/projects/glucose-ring` with a real `.gitignore`
  (build artifacts, test binaries, Android `build/`, `local.properties`).
- `~/.git` removed.
- **Checked before deleting**, since it was irreversible: 2 commits, every
  tracked file also present on disk (nothing history-only), no stashes, no
  tags, no remote-tracking refs. A 152 KB tarball was taken first.
- **One thing discarded worth recording:** `~/.git` had a remote,
  `https://github.com/Alm1973/robotic-vision-project.git`, never pushed to.
  That association is gone. Re-add it to a proper repo if you still want it.
- **Consequence:** `~/tiktok-streak` is no longer in a git repo — it never had
  its own `.git`, it only looked tracked because it sat inside `~/.git`'s
  worktree. Nothing there was tracked, so no history was lost. It has its own
  `.gitignore` and looks like it was meant to be a repo; I did **not**
  initialise one unasked.

### 3. Watchdog vs DFU — hard gate recorded

- [README.md](README.md) §7 now carries an explicit **HARD GATE**: prove
  OTA/BLE DFU works with `WATCHDOG_ENABLE=1` before sealing the enclosure or
  removing USB-C.
- **New finding that sharpens your point:** the serial DFU recipe uses
  `--singlebank` (`platform.txt:143`), which **erases the application before
  writing the replacement**. A watchdog reset partway through therefore leaves
  no valid application at all — not a retry, a brick. Your instinct that fast
  UF2 is probably fine and slow BLE DFU is the real question is exactly right,
  and singlebank is why the downside is so asymmetric.

### 4. Charge-detect polarity — now a hard gate

Bring-up step 6 ([README.md](README.md) §9) is marked **HARD GATE**, with the
reasoning stated inline: the guard fails *open* if polarity is inverted, which
is precisely the P0.31 scenario it exists to prevent, and a guard that reads
backwards is worse than no guard because you will trust it. Confirm `chg=1` on
USB and `chg=0` off it **before attaching a cell**.

### 5. xDrip+ intent strings — log-first, trust-later

- [PHONE_APP.md](PHONE_APP.md) leads with a **Step 0**: dump the entire extras
  bundle to logcat and confirm every key before writing forwarding logic.
- Enforced in code, not just prose: `XdripReceiver.DUMP_EXTRAS` ships **`true`**
  ([android/app/src/main/java/com/glucosering/ring/XdripReceiver.kt](android/app/src/main/java/com/glucosering/ring/XdripReceiver.kt)), and the
  receiver logs a warning when the expected extras are missing rather than
  failing silently.

### Your decisions, implemented

| Decision | Result |
|---|---|
| Fork `BLEUart` — **no** | Not done. Threat model agreed: worst case is someone within ~10 m making the ring buzz; it cannot reach a pump or data. |
| Install Android toolchain — **yes** | Done, and the app **compiles**. See below. |
| Watchdog reset should buzz — **yes** | Distinct long-short-long fault pattern at boot, full drive ([glucose-ring.ino:164](glucose-ring.ino#L164), fired at [:885](glucose-ring.ino#L885)). |
| Low-battery buzz while charging — **no** | Unchanged; battery handling stays fully suppressed on the charger. |
| Cap stale alerting — **yes** | `stale_max_repeats`, default 6 (~30 min). On hitting the cap: distinct terminal pattern, `#ALERT stale_giveup`, then silence until fresh data. `!SET STALEREPEATS 0` restores unlimited. |
| `VBAT_FLOOR` 3300 mV — **leave** | Unchanged. |
| ASan — **not worth it** | Dropped; UBSan plus sentinel-byte bounds checks retained. |

On the stale cap specifically — your reasoning is now recorded in the code and
the README, because it is the kind of decision that looks wrong later without
it: the giving-up signal is deliberately distinct so the silence afterwards
reads as *"it stopped trying"*, not *"it got fixed"*.

### Phase 3 — Android app, built

- Toolchain: `android-commandlinetools`, SDK platform 34, build-tools 34.0.0.
- **Homebrew's `gradle` is 9.7.1 on JDK 26 and no AGP supports either.** The
  project pins a **Gradle 8.11.1 wrapper**, AGP 8.7.3, Kotlin 2.0.21, and
  points `org.gradle.java.home` at the JDK 21 already installed. Build with
  `./gradlew`, never with `gradle`.
- `RingProtocol.kt` and `Reading.kt` are pure Kotlin, no Android imports —
  the same split that makes the firmware's parser host-testable. **22 JVM
  tests**, using the vectors from PROTOCOL.md §8, i.e. the same table the
  firmware's own tests assert. Both ends are checked against one spec.
- `NusClient.kt` serialises GATT operations through a queue (Android permits
  one outstanding op per connection and silently drops the rest) and chunks to
  the negotiated MTU.
- Output: `app-debug.apk`, 3.2 MB, minSdk 26 / targetSdk 34.

---

## FAILED

Nothing failed in this session.

Carried forward, unchanged and accepted per your call:

**AddressSanitizer is unusable on this machine.**
```
AddressSanitizer: CHECK failed: sanitizer_malloc_mac.inc:189
"((!asan_init_is_running)) != (0)" (0x0, 0x0)
```
Apple clang 14.0.3 on Darwin 25.6. Not pursued further, per your call.

**GATT-level encryption on NUS — abandoned by decision, not by defeat.**
`BLEUart::begin()` hardcodes `SECMODE_OPEN`; `setPermission()` only fills
`_attr_meta`, already consumed by `begin()`. Reachable only by forking library
internals. You said no; agreed.

---

## SKIPPED

- **No upload, no phone install.** No hardware.
- **Did not `git init` `~/tiktok-streak`** — you didn't ask, and creating repos
  unasked is a filesystem change you didn't request.
- **Did not fork `BLEUart::begin()`** — your decision.
- **Did not chase ASan** — your decision.
- **Did not install Android Studio**, only the command-line tools: the cask
  needs GUI wizard steps to obtain an SDK, whereas the CLI path is fully
  scriptable and sufficient to compile.
- **No device scanning UI** in the app. `MainActivity` requests permissions and
  starts the service; device selection is a stub. The ring advertises as
  `GlucoseRing`.
- **No config UI** in the app. Everything in PROTOCOL.md §4 is reachable, but
  only the protocol layer is written.

---

## ASSUMED

**A1. Bonding is enforced in firmware, not by the SoftDevice.** An unpaired
device can still connect and write; the ring discards the bytes. Accepted
deliberately — your threat-model call.

**A2. The xDrip+ intent action and extra keys are unverified.** No xDrip+
source or APK on this machine. `DUMP_EXTRAS = true` ships enabled so this gets
checked against reality before it wastes an evening.

**A3. `~CHG` is open-drain and active-low.** `variant.cpp:40` names the pin and
identifies the BQ25100 but states neither polarity nor drive type. Now a hard
gate in bring-up.

**A4. `VBAT_DIVIDER_COMP = 1510/510` is nominal, not measured.**

**A5. The battery percentage curve is invented** — generic Li-ion shape, not
your cell. Display only; the cutoff uses raw mV.

**A6. `stale_max_repeats = 6` is my number, not yours.** You said cap it; you
did not say where. Six repeats at the default 5-minute interval is ~30 minutes
of buzzing before it gives up. Change it with `!SET STALEREPEATS`.

**A7. The fault and give-up buzz patterns are guesses at distinguishability.**
Long-short-long for fault, three ticks then a long for give-up. Whether they
are actually tellable apart from the glucose patterns through a ring band is
an ergonomics question only wearing it answers.

**A8. PWM intensity assumes a 25 ms stiction kick is needed.**

**A9. PWM release sequence inferred from `HardwarePWM.cpp`** — ordering
verified against source; that the peripheral actually stops drawing current is
not.

**A10. D10 works as a plain output despite being `PIN_SPI_MOSI`** — nothing
claims the pin unless `SPI.begin()` is called, which never happens.

**A11. Slave latency 4 assumes the phone accepts our parameters.**

**A12. `sd_power_usbregstatus_get()` is used because `NRF_POWER` is restricted
under the SoftDevice** — verified at `nrf_soc.h:586` (S140 7.3.0).

**A13. The board menu key `s140v6` selects SoftDevice 7.3.0**
(`boards.txt:102-104`).

**A14. `millis64()` is not reentrant** — `loop()` and same-task callbacks only.

**A15. Debounced autosave can lose up to 5 s of config change** to a power cut.

**A16. Schema bumped 1 → 2** for `stale_max_repeats`. A v1 config file still
loads and picks up the default; a *newer* schema is rejected rather than
guessed at. Tested.

**A17. Gradle/AGP/JDK versions were chosen to be compatible, not because they
are your preference.** AGP 8.7.3 + Gradle 8.11.1 + JDK 21 is a combination
that works; the machine's own `gradle` (9.7.1) and default JDK (26) do not.
`android/local.properties` is gitignored, so `sdk.dir` must be recreated
elsewhere.

**A18. `minSdk 26` is my choice** — runtime-registered receivers behave
predictably from there, and BLE is stable. Anything older needs more work.

**A19. The Kotlin app is compiled, not exercised.** No emulator, no device.
Only the pure protocol layer has tests; the service, GATT client and receiver
have never run.

---

## UNVERIFIABLE WITHOUT HARDWARE

1. **Watchdog vs DFU**, now the hard gate. `--singlebank` makes a mid-flash reset unrecoverable.
2. **Charge-detect polarity** (A3) — guard fails open if wrong.
3. **That the VBAT guard prevents the P0.31 hazard at all.** I implemented the guard you specified; the headers document pins, not failure modes.
4. **The motor being driven on D10.** Compiling proves the pin maps and the PWM path resolves; nothing about gate drive.
5. **Divider calibration** (A4) and the true cutoff point.
6. **Idle current, and every battery-life figure.**
7. **Whether the motor starts from the configured duty** (A8).
8. **Whether the fault / give-up patterns are distinguishable on a finger** (A7).
9. **Whether the phone grants the requested connection parameters** (A11).
10. **Bonding end-to-end.**
11. **The entire Android runtime path** (A19) — GATT, bonding, chunking, reconnect, foreground-service survival under OEM power management.
12. **The xDrip+ contract** (A2).

---

## OUT OF SCOPE FOR YOU

- Flashing or running anything on the board; installing the APK on a phone.
- Physical assembly — MOSFET, pulldown, diode, motor harness, ring form factor, and coupling the motor so vibration carries through the band.
- Any current or voltage measurement.
- Deploying Nightscout — signup, hosting, MongoDB, env vars.
- Anything needing a browser login: Nightscout hosting, Dexcom/LibreLink accounts, Play publishing.
- Installing and configuring xDrip+, including "Broadcast locally".
- Confirming the xDrip+ intent contract (A2).
- Pairing the ring to a phone.
- **Reading your Dexcom app's alert values** — see the first open question.
- The clinical decision itself.

---

## OPEN QUESTIONS

1. **Thresholds — I still cannot set them, and I want to flag why clearly.**
   You said to use the low and high alert values already configured in your
   Dexcom app. That is a sound way to choose them, and it resolves *who
   decides* — but **I have no way to read your phone**, so I do not know the
   two numbers. Tell me the values and I'll set the compile-time defaults, or
   send `!SET THRESH <low> <high>` once the ring is up and it persists to
   flash. Until then both sides are `unset` and the ring will not alert on
   glucose at all.
2. **`stale_max_repeats = 6`** (A6) — my number, roughly 30 minutes. Say if you
   want a different ceiling.
3. **Are the fault and give-up patterns distinct enough?** (A7) Only wearing it
   answers this; easy to change once you know.
4. **Do you want a `git init` in `~/tiktok-streak`?** It has a `.gitignore` and
   looks like it was meant to be a repo.
5. **Next on the app:** device scanning/pairing UI, or the config screen?
   Neither exists. Scanning is the blocker for using it at all.

---

## BUILD STATUS

### Firmware — clean compile

```
$ PATH="$PWD/tools/bin:$PATH" LC_ALL=C.UTF-8 LANG=C.UTF-8 \
  arduino-cli compile --clean --fqbn Seeeduino:nrf52:xiaonRF52840 .
exit: 0

glucose-ring.ino:38:4: warning: #warning "Threshold defaults are placeholders -
  that side never alerts until set (compile-time or !SET THRESH)." [-Wcpp]

Sketch uses 135324 bytes (16%) of program storage space. Maximum is 811008 bytes.
Global variables use 14604 bytes (6%) of dynamic memory, leaving 222964 bytes
for local variables. Maximum is 237568 bytes.
```

The only warning is the intentional threshold placeholder — which is currently
also the most important open item.

### Firmware — host tests

```
$ ./tests/run_tests.sh
exit: 0
passed: 343   failed: 0
```

Up from 315: new coverage for `stale_max_repeats` defaults, bounds, `0` as
"never stop", serialisation round-trip, v1-config forward compatibility, the
`STALEREPEATS` command surface, and the new `fault` / `giveup` buzz names.
Built with `-Wall -Wextra -Wconversion -Wshadow -Wpedantic
-fsanitize=undefined`. Zero warnings, zero UBSan reports.

### Android app

```
$ cd android && ANDROID_HOME=/opt/homebrew/share/android-commandlinetools \
  ./gradlew :app:assembleDebug :app:testDebugUnitTest
exit: 0
BUILD SUCCESSFUL

  android unit tests: 22 run, 0 failures, 0 errors

app/build/outputs/apk/debug/app-debug.apk   3,199,456 bytes
package: com.glucosering.ring  versionName='0.1.0'
sdkVersion:'26'  targetSdkVersion:'34'  compileSdkVersion='34'
```

### Not run

`arduino-cli upload` — no hardware.
`adb install` — no phone attached, and not asked for.
