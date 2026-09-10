# glucose-ring — work report

Hardware has not arrived. Everything below was verified by **compiling** and
by **host-side tests**. Nothing was uploaded.

**Correction up front:** the brief said `MOTOR_PIN` moves "from D0 to D10".
It was on **D3**, not D0 ([glucose-ring.ino:75](glucose-ring.ino#L75) — was line 64). D0 appeared
nowhere in the sketch; the only mention was a README parenthetical listing
free pins. The move to D10 is done and your board-edge reasoning holds
regardless — this only matters if you were picturing a different layout.

**Phase 2 was already complete** from the previous session. I re-verified it
rather than rebuilding it; nothing there was rewritten except what the pin
move touched.

---

## DONE

### Phase 1 — motor pin D3 → D10

- [glucose-ring.ino:75](glucose-ring.ino#L75) — `#define MOTOR_PIN D10`, with the rationale, the SPI tradeoff, and the verification recorded in the comment block above it ([:56–72](glucose-ring.ino#L56)).
- README: ASCII wiring diagram, pin table, and bring-up step 1 (`D10→GND should read ~10k`) all updated. **Project-wide sweep confirms zero remaining `D3` references** in sketch, headers, or docs.
- Verified against the core, not assumed:
  - `variant.cpp:19` — `47, // D10 is P1.15 (MOSI)`; 47 decodes as port 1, pin 15.
  - `HardwarePWM::_set_psel` writes `g_ADigitalPinMap[pin]` straight into `PSEL.OUT[ch]`, which is the port|pin encoding — so **PWM intensity works on P1.15**. This was worth checking: a pin that works for `digitalWrite` does not automatically work for the PWM path.
  - P1.15 is neither an NFC pin (P0.09/P0.10) nor RESET.
  - `variant.h:93` defines D10 as `PIN_SPI_MOSI` — taking it rules out the default `SPI` object. Nothing here uses SPI; noted in the comment and the README table.

### Phase 2 — re-verified, not rebuilt

| # | Item | Where |
|---|---|---|
| 1 | VBAT guard while charging — `~CHG` (P0.17) **and** USB VBUS; also suppresses low-battery shutdown while charging | [:339](glucose-ring.ino#L339), [:416](glucose-ring.ino#L416) |
| 2 | Persistent config in LittleFS; defaults on first boot; corrupt file rejected and rewritten | [:481](glucose-ring.ino#L481), [:463](glucose-ring.ino#L463) |
| 3 | BLE command channel over NUS, routed by first byte | [:629](glucose-ring.ino#L629), [command.cpp:75](command.cpp#L75) |
| 4 | Battery Service 0x180F | [:151](glucose-ring.ino#L151) |
| 5 | Re-advertise on disconnect; interval/latency tuned, tradeoff commented | [:819](glucose-ring.ino#L819), [:806](glucose-ring.ino#L806), comment at [:104](glucose-ring.ino#L104) |
| 6 | Bonding gate — unencrypted bytes discarded | [:540](glucose-ring.ino#L540) |
| 7 | Watchdog, 30 s, `CONFIG.SLEEP=Run`, reset reported as `#BOOT wdt_reset=1` | [:210](glucose-ring.ino#L210) |
| 8 | Pure parser + host tests — **315 assertions, 0 failures** | [reading_parser.cpp:30](reading_parser.cpp#L30), `tests/` |
| 9 | PROTOCOL.md | [PROTOCOL.md](PROTOCOL.md) |
| 10 | README + 23-step bring-up checklist | [README.md](README.md) §9 |

Details for these are unchanged from the previous report; the table is here
so this document stands alone.

### Phase 3 — specification only

- [PHONE_APP.md](PHONE_APP.md) — permissions (including the API-31/33/34 traps
  that silently break BLE foreground services), the xDrip+ intent contract,
  BLE client flow in required order, MTU chunking and newline reassembly,
  reconnect with backoff, foreground-service survival, and acceptance tests.
- Two hazards specified there that the firmware's strictness would otherwise
  turn into silent data loss: **a comma in the xDrip+ direction string** would
  create a fourth field and get the whole reading rejected, and **xDrip+
  re-broadcasts duplicate readings**, which the ring drops as not-newer.

---

## FAILED

Nothing failed in this session. Two items carried from the previous one
remain true:

**1. AddressSanitizer is unusable on this machine.**

```
AddressSanitizer: CHECK failed: sanitizer_malloc_mac.inc:189
"((!asan_init_is_running)) != (0)" (0x0, 0x0)
```

Apple clang 14.0.3 on Darwin 25.6; its ASan runtime cannot intercept this
macOS's malloc zones. Reproduced with both `clang++` on PATH and
`/usr/bin/clang++` (the same binary). UBSan is enabled instead, and the tests
carry explicit sentinel-byte bounds checks.

**2. GATT-level encryption on NUS — attempted, abandoned.**
`BLEUart::begin()` hardcodes `SECMODE_OPEN` on both characteristics and
`setPermission()` only fills `_attr_meta`, which `begin()` has already
consumed. Not reachable without forking library internals. See ASSUMED §A1.

---

## SKIPPED

- **No upload.** As instructed.
- **Did not scaffold the Kotlin app.** Toolchain absent; scaffolding source I could not compile would be unverified code presented as work. Spec written instead.
- **Did not install the Android SDK, Gradle, or Android Studio.** Explicitly instructed not to.
- **Did not rebuild Phase 2.** It was already complete; re-verified by compile + tests.
- **Did not fork `BLEUart::begin()`** for encrypted characteristics — ~20 lines of copied library internals that would silently diverge on a core update. Open question.
- **Did not change charge current** (`HICHG`/P0.13); the power-on default is the safer setting and firmware cannot go lower.
- **No commit.** Nothing has been committed since `83d52be` (the v1 firmware); all v2 work and this session's changes are uncommitted.

---

## ASSUMED

Every judgement call, and everything I could not verify.

**A1. Bonding is enforced in firmware, not by the SoftDevice.** *(most important)*
An unpaired device **can still connect and write** to NUS RX; the GATT layer
accepts it. The ring discards those bytes, so the practical attack surface —
buzzing the motor, reading state, changing thresholds — is closed. But this
depends on my code path being correct rather than on the radio stack refusing
the write. Weaker than real GATT enforcement.

**A2. The xDrip+ intent action and extra keys in PHONE_APP.md are unverified.**
There is no xDrip+ source or APK on this machine. Those strings come from
knowledge of the codebase, not inspection. **If a key name is wrong the
receiver silently never fires** — the worst failure mode, indistinguishable
from "no readings yet". Confirm against xDrip+'s `Intents.java` and log the
whole extras bundle on first run.

**A3. `~CHG` is open-drain and active-low.** `variant.cpp:40` names the pin and
identifies the BQ25100 but says nothing about polarity or drive type.
Active-low is what `~` denotes and what the BQ25100 datasheet specifies;
open-drain is why I enable a pull-up. **Neither is confirmed from the
headers.** If it is push-pull or active-high, charge detection inverts and the
VBAT guard fails open. Bring-up step 6 exists to catch this.

**A4. `VBAT_DIVIDER_COMP = 1510/510` is nominal, not measured.** The cutoff is
only as accurate as this constant. Calibration is checklist step 8.

**A5. The battery percentage curve is invented.** A generic 8-point Li-ion
shape, not your cell's discharge curve. Display only; the cutoff uses raw mV.

**A6. `VBAT_FLOOR_MV = 3300` is my choice**, not derived from your cell's
datasheet.

**A7. PWM intensity assumes a stiction kick is needed** — 25 ms at full drive
before dropping to the configured duty. The 25 ms is a guess.

**A8. PWM release sequence inferred from reading `HardwarePWM.cpp`** —
`removePin` → `stop` → `releaseOwnership`. The ordering is verified against
source; **that the peripheral actually stops drawing current is not.**

**A9. D10 works as a plain output despite being `PIN_SPI_MOSI`.** Nothing in
the core claims the pin unless `SPI.begin()` is called, and we never call it.
Verified by inspection of `variant.h`, not by running.

**A10. Slave latency 4 assumes the phone accepts our parameters.** Centrals may
refuse or renegotiate. Values sit inside Apple's guidelines to make acceptance
likely; `BLEConnection::getSlaveLatency()` exists if you want to log what was
actually granted.

**A11. `sd_power_usbregstatus_get()` is used because `NRF_POWER` is restricted
once the SoftDevice is enabled** — verified at
`s140_nrf52_7.3.0_API/include/nrf_soc.h:586`. `RESETREAS` is read directly,
but only *before* `Bluefruit.begin()`.

**A12. The board menu key `s140v6` actually selects SoftDevice 7.3.0**
(`boards.txt:102-104`). I used the 7.3.0 headers.

**A13. `RX_LINE_MAX = 96` is sized by inspection**, not a computed worst case.
An underestimate degrades to "that command is rejected", not to a buffer
problem.

**A14. `millis64()` is not reentrant** — called only from `loop()` and from
`CmdHost` callbacks on the same task.

**A15. Debounced autosave can lose up to 5 s of config change** to a power cut.
Deliberate trade against flash wear; `!SAVE` forces a write.

**A16. `#ALERT` fires on transitions only**, so a phone counting `#ALERT` lines
undercounts actual buzzes.

**A17. Config is text, not a packed struct** — larger and slower, survives
layout changes, inspectable. Deliberate.

---

## UNVERIFIABLE WITHOUT HARDWARE

Ranked by how badly a wrong guess bites.

1. **Watchdog vs. DFU.** The nRF52 WDT cannot be stopped once started and is not cleared by a soft reset. Whether the UF2 bootloader feeds it is not determinable from the installed core — the bootloader is prebuilt. If it does not, **entering DFU may reset mid-upload**. Flash first with `-DWATCHDOG_ENABLE=0`.
2. **Charge-detect polarity** (A3) — the VBAT guard fails open if wrong.
3. **That the VBAT guard prevents the P0.31 hazard at all.** I implemented the guard you specified; the headers document pins, not failure modes.
4. **The motor actually being driven on D10.** Compiling proves the pin maps and PWM path are valid; it proves nothing about the gate drive. `-DTEST_MODE=1` on real hardware is the check.
5. **Divider calibration** (A4) and the true cutoff point.
6. **Idle current, and therefore every battery-life figure.** Including whether PWM release really drops the peripheral's draw (A8) and whether the divider high-Z release is real.
7. **Whether the motor starts from the configured duty** — stiction is motor- and load-specific (A7).
8. **Whether the phone grants the requested connection parameters** (A10).
9. **Bonding end-to-end** — pairing completes, keys persist across reboot, `secured()` flips when expected.
10. **Flash endurance** under real `!SET` traffic.
11. **Whether the buzz patterns are distinguishable on a finger.** Distinct on paper; an ergonomics question only wearing it answers.
12. **The entire phone app** — unbuilt and uncompiled.

---

## OUT OF SCOPE FOR YOU

Work this project needs that I cannot do from this machine.

- **Flashing and running anything on the board.** No hardware, and instructed not to upload.
- **Physical assembly** — soldering the AO3400, the 10k pulldown, the flyback diode, the motor harness, and fitting it all into a ring form factor. Also the mechanical question of coupling the motor so vibration is felt through the band.
- **Any current or voltage measurement.** Idle current, divider calibration, cutoff verification, PWM release — all need a meter on real silicon.
- **Installing the Android toolchain.** Requires downloads and accepting SDK licences; you told me not to install.
- **Deploying Nightscout.** Account signup, hosting provider, MongoDB, environment variables — all browser logins I can neither perform nor should.
- **Anything needing a browser login**: Nightscout hosting, Dexcom/LibreLink accounts, Google Play publishing, xDrip+ Play/APK install.
- **Installing and configuring xDrip+ on the phone**, including turning on "Broadcast locally", which the whole Phase 3 design depends on.
- **Confirming the xDrip+ intent contract** (A2) — needs the actual app or its source.
- **Pairing the ring to a phone** and everything downstream of that.
- **The clinical decision**: choosing `LOW_MG_DL` / `HIGH_MG_DL`. Not mine to make, and the firmware is built so it never quietly becomes mine.

---

## OPEN QUESTIONS

1. **Thresholds are still `unset`, so the ring never alerts on high or low.** This is the single thing standing between the current firmware and it doing its job. Give me numbers, or set them at runtime with `!SET THRESH`.
2. **Fork `BLEUart::begin()` for real GATT encryption?** (A1) My recommendation is still **no** — the firmware gate closes the practical attack surface, and a silent divergence in a security path on a core update is worse than the gap it closes. Say the word and I'll do it.
3. **Install the Android toolchain?** One `brew install --cask android-studio` plus opening it once unblocks Phase 3. I won't run it without you saying so.
4. **`VBAT_FLOOR_MV = 3300`** (A6) — confirm against your cell's datasheet.
5. **Should a watchdog reset buzz?** Currently reported over BLE but silent to the wearer. A fault that restarted the ring is arguably something to feel.
6. **Should a low-battery warning still buzz while charging?** Right now battery handling is fully suppressed on the charger.
7. **Cap on stale alerting?** The staleness clock starts at boot, so a ring left in a drawer buzzes every `repeat` minutes indefinitely. Intentional — silence is dangerous — but you may want a ceiling.
8. **mg/dL only.** If you need mmol/L it belongs in the phone app; say so if the ring should know.
9. **Commit?** Nothing since `83d52be`. Note your **home directory is a git repo with no `.gitignore`** — `~/.ssh`, `~/.claude.json` and `~/.zsh_history` are untracked but not ignored, so I've only ever staged explicit paths. Worth fixing separately.

---

## BUILD STATUS

### Firmware — clean compile, after the D10 move

```
$ PATH="$PWD/tools/bin:$PATH" LC_ALL=C.UTF-8 LANG=C.UTF-8 \
  arduino-cli compile --clean --fqbn Seeeduino:nrf52:xiaonRF52840 .
arduino-cli exit: 0

/Users/shauryakhidake/projects/glucose-ring/glucose-ring.ino:38:4: warning:
  #warning "Threshold defaults are placeholders - that side never alerts
  until set (compile-time or !SET THRESH)." [-Wcpp]

Sketch uses 133716 bytes (16%) of program storage space. Maximum is 811008 bytes.
Global variables use 14604 bytes (6%) of dynamic memory, leaving 222964 bytes
for local variables. Maximum is 237568 bytes.
```

The only warning is the intentional threshold placeholder.

### Firmware — bring-up variant

```
$ arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840 \
    --build-property "compiler.cpp.extra_flags=-DTEST_MODE=1 -DDEBUG_SERIAL=1 \
                      -DWATCHDOG_ENABLE=0 -DREQUIRE_BONDING=0" .
variant exit: 0
Sketch uses 135188 bytes (16%) of program storage space. Maximum is 811008 bytes.
```

### Host tests

```
$ ./tests/run_tests.sh
tests exit: 0

glucose-ring host tests (parser / config / command protocol)

-- parser: valid input
-- parser: malformed
-- parser: truncated
-- parser: out of range
-- parser: garbage bytes / direction hygiene
-- parser: API contract
-- config: defaults and validation
-- config: serialise round trip
-- config: deserialise robustness
-- config: pattern grammar
-- command: routing
-- command: query
-- command: set
-- command: actions
-- command: errors and hostile input
-- command: response buffer safety

=====================================
passed: 315   failed: 0
=====================================
```

Built with `-Wall -Wextra -Wconversion -Wshadow -Wpedantic
-fsanitize=undefined`. Zero compiler warnings, zero UBSan reports.

### Phase 3

Not built. No Android SDK, Gradle, `kotlinc`, or Android Studio on this
machine; `ANDROID_HOME` and `ANDROID_SDK_ROOT` unset. OpenJDK 22.0.2 is
present but is not sufficient on its own.

### Not run

`arduino-cli upload` — no hardware, and instructed not to.
