# glucose-ring

Haptic CGM alert ring on a **Seeed XIAO nRF52840** (plain BLE, *not* Sense).
A phone app pushes glucose values over Nordic UART Service; the ring buzzes a
coin motor on low / high / stale. Thresholds, timings and buzz patterns are
configurable over BLE and persist in flash.

> **This is a secondary alert.** A flat cell, a dropped BLE link, or a hung
> sketch all fail *silently* from the wearer's point of view. Keep your
> phone's own CGM alarms on.

- **Wire protocol:** [PROTOCOL.md](PROTOCOL.md) — read this if you are writing the phone app.
- **Build/verification status and caveats:** [REPORT.md](REPORT.md).

---

## 1. Set your thresholds

Both thresholds ship as `RC_UNSET`. **An unset side never alerts.** Two ways
to set them:

- **Over BLE, no reflash:** `!SET THRESH 70 200` — persists to flash.
- **At compile time:** edit `DEFAULT_LOW_MG_DL` / `DEFAULT_HIGH_MG_DL` in
  [glucose-ring.ino](glucose-ring.ino). These seed the config on first boot only.

Every build warns until the compile-time defaults are real numbers. Picking
them is a clinical decision, not a firmware one.

---

## 2. Wiring

The XIAO pin **only ever drives a MOSFET gate**. The motor's ~85 mA never
touches the nRF52.

```
  XIAO D10 ──[ 1k ]──┬── gate  (AO3400)          1k is optional; it tames
                     │                            the gate edge
                    10k
                     │
                    GND        ← pulldown holds the gate OFF from power-on
                                 until setup() runs. Do not omit this.

   BAT+ (battery pad) ──┬── motor + ──┬── motor − ── AO3400 drain
                        │             │
                        └──|◀─────────┘   1N4148, cathode (band) to BAT+

   AO3400 source ── GND
```

| Signal | Pin | Note |
|---|---|---|
| Motor gate | **D10** (P1.15) | Same board edge as GND and 3V3, so the motor harness never crosses the board. D10 is the default SPI MOSI, so taking it rules out the `SPI` object — nothing here needs it. |
| Motor supply | `BAT+` pad | Straight off the cell, not 3V3. |
| Ground | `GND` | MOSFET source and motor return |
| Battery sense | *internal* | P0.31 via onboard 1M/510k divider, gated by P0.14 |
| Charge status | *internal* | P0.17 (`~CHG`), active low, from the BQ25100 |

**Diode orientation matters.** Band (cathode) to `BAT+`. Backwards it is a
dead short across your cell.

### Battery

- **The divider is pulsed, not held.** `VBAT_ENABLE` (P0.14) goes low only for
  the ~6 ms of a reading, once a minute, then is released to **high-Z** — not
  driven high. Holding it low is the classic XIAO power bug.
- **Sampling is skipped while charging.** Reading the divider while the
  charger drives the BAT node is a documented hazard on P0.31. The firmware
  checks `~CHG` (P0.17) *and* USB VBUS and skips the read if either says
  external power is present. It also never shuts down on low battery while on
  the charger.
- **Calibrate `VBAT_DIVIDER_COMP`.** Set to the nominal `1510/510`. Build with
  `-DDEBUG_SERIAL=1`, compare against a multimeter, and scale. The cutoff is
  only as good as this number.
- `VBAT_FLOOR_MV` (3300) triggers permanent SYSTEM OFF after 5 consecutive
  confirmations. Only a reset or charge brings it back.
- **Charge current vs. a 40 mAh cell:** the board's default is already above
  1C for a 40 mAh cell, and firmware cannot go lower. Consider a larger cell
  or charging externally.

---

## 3. Toolchain

```bash
brew install arduino-cli
```

```bash
arduino-cli config init && arduino-cli config add board_manager.additional_urls https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
```

```bash
arduino-cli core update-index && arduino-cli core install Seeeduino:nrf52
```

**FQBN: `Seeeduino:nrf52:xiaonRF52840`** — the `Seeeduino:nrf52` core, not
`Seeeduino:mbed` (Bluefruit only exists in the former). SoftDevice is S140
**7.3.0**, despite the board menu key being named `s140v6`.

### Two environment fixes you will need

Both are Seeed core packaging problems on modern macOS, not bugs in this
sketch. Both are handled in the build commands below.

1. **`exec: "python": executable file not found in $PATH`** — the core's
   post-build step hardcodes `python`; macOS ships only `python3`.
   `tools/bin/python` here is a shim.
2. **`RuntimeError: Click will abort ... ASCII`** — the bundled
   `adafruit-nrfutil` is a frozen Click binary that refuses to run under an
   ASCII locale, and this shell has `LANG` unset.

Permanent fix instead:

```bash
mkdir -p ~/.local/bin && ln -sf "$(which python3)" ~/.local/bin/python && printf 'export LANG=C.UTF-8\nexport LC_ALL=C.UTF-8\nexport PATH="$HOME/.local/bin:$PATH"\n' >> ~/.zshrc
```

---

## 4. Build

```bash
cd ~/projects/glucose-ring && PATH="$PWD/tools/bin:$PATH" LC_ALL=C.UTF-8 LANG=C.UTF-8 arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840 .
```

Clean rebuild (recompiles core + SoftDevice, slow):

```bash
cd ~/projects/glucose-ring && PATH="$PWD/tools/bin:$PATH" LC_ALL=C.UTF-8 LANG=C.UTF-8 arduino-cli compile --clean --fqbn Seeeduino:nrf52:xiaonRF52840 .
```

### Compile flags

| Flag | Default | Purpose |
|---|---|---|
| `TEST_MODE` | 0 | Cycle every buzz pattern on boot, no phone needed |
| `DEBUG_SERIAL` | 0 | USB serial tracing (costs power) |
| `WATCHDOG_ENABLE` | 1 | Set 0 during bring-up if DFU misbehaves (§7) |
| `REQUIRE_BONDING` | 1 | Set 0 **only** on the bench (§6) |

```bash
cd ~/projects/glucose-ring && PATH="$PWD/tools/bin:$PATH" LC_ALL=C.UTF-8 LANG=C.UTF-8 arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840 --build-property "compiler.cpp.extra_flags=-DTEST_MODE=1 -DDEBUG_SERIAL=1" .
```

`compiler.cpp.extra_flags` *replaces* the property, so pass all `-D`s in one string.

---

## 5. Host tests

The parser, config store and command protocol are pure C++ with no Arduino
dependency, so they are fully testable on the Mac. This is the only part
verifiable without hardware — treat it as the real safety net.

```bash
cd ~/projects/glucose-ring && ./tests/run_tests.sh
```

Covers valid input, malformed input, truncated input (every prefix of a good
line), out-of-range values, garbage/high/control bytes, NULL contracts,
non-mutation of caller buffers, config round-tripping and corrupt-file
rejection, the full command surface, and response-buffer bounds at every
size from 1 to 24 bytes.

The tests link the *same* `.cpp` files the firmware compiles — not copies.

> ASan is deliberately not enabled: Apple clang 14.0.3's ASan runtime aborts
> at init on this macOS (Darwin 25.x). UBSan is on, and the tests carry
> explicit sentinel-byte overflow checks. Re-add ASan on a newer toolchain.

---

## 6. Security / pairing

The ring requests pairing on connect and **discards every byte from an
unencrypted link** — no commands, no readings, not even a version string.

**What this makes harder during development:**

- You must bond before nRF Connect can do anything. An unbonded session
  connects and appears to write successfully, but nothing happens.
- After changing the GATT table, phones cache services. **Forget the device
  in the phone's Bluetooth settings** and re-pair, or you will chase ghosts.
- Bond keys live in the same LittleFS as the config (`/adafruit/bond_prph`).
  A filesystem format drops bonds; re-pair afterwards.
- For quick bench work, build with `-DREQUIRE_BONDING=0`. **Never wear it
  that way** — any nearby device could buzz it.

**What this does *not* do:** the NUS characteristics still advertise open
GATT permissions, because this core's `BLEUart::begin()` hardcodes
`SECMODE_OPEN` and overwrites anything set beforehand. An attacker can still
connect and write; the writes are simply ignored. See REPORT.md → ASSUMED.

---

## 7. Watchdog

A 30 s hardware watchdog (nRF52 WDT, `CONFIG.SLEEP=Run` so it keeps counting
through WFI) resets the ring if `loop()` stops running. On the next
connection the ring reports `#BOOT ... wdt_reset=1` so a recovered hang is
visible rather than silent.

**Bring-up caveat:** once started, the nRF52 WDT cannot be stopped or
reconfigured, and it is not cleared by a soft reset. If the UF2 bootloader
does not feed it, entering DFU could reset mid-upload. **This is unverified
without hardware.** Flash first with `-DWATCHDOG_ENABLE=0`, then re-enable.

The serial DFU recipe uses `--singlebank` (`platform.txt:143`), which erases
the application before writing the new one. A watchdog reset partway through
that leaves **no valid application** — so this is not merely an annoyance.

### HARD GATE — do not seal the ring until this passes

> **Prove OTA/BLE DFU works with `WATCHDOG_ENABLE=1` before sealing the
> enclosure or removing the USB-C connector.**
>
> A 30 s watchdog against a fast UF2 upload over USB is probably fine. Against
> a **slow BLE DFU** it is a real question: if the transfer or the bootloader's
> pre-write erase takes longer than the watchdog between feeds, the ring
> resets mid-flash. With `--singlebank` that means a device with no valid
> application — and with USB-C removed and the case sealed, no way back in.
>
> Sequence: enable the watchdog, perform a **full OTA DFU over BLE**, confirm
> the ring boots the new firmware, and repeat it at least once. Only then is
> the thin/sealed build path viable. If OTA DFU proves unreliable with the
> watchdog on, the options are a longer `WATCHDOG_TIMEOUT_S`, disabling the
> watchdog immediately before a DFU handoff, or keeping USB-C.

---

## 7b. Alert behaviour and buzz patterns

| Condition | Pattern | Configurable |
|---|---|---|
| **Low** | 3 x 400 ms — long and urgent | yes (`!SET PATTERN low`) |
| **High** | 2 x 120 ms | yes |
| **Stale** | 2 x 60 ms, deliberately gentler | yes |
| **Battery** | 600/300/150 ms fade, always full drive | no |
| **Fault** (watchdog reset) | 150/60/150 ms long-short-long, at boot, full drive | no |
| **Gave up** (stale cap hit) | three 60 ms ticks then one 400 ms | no |

An active alert re-buzzes every `repeat` minutes (default 5). A change of
state buzzes immediately. Stale outranks the thresholds: once the value is
not current the ring will not act on it in either direction.

**Stale alerting is capped.** After `stale_repeats` consecutive stale buzzes
(default 6 — about 30 minutes at the default repeat interval), the ring buzzes
the distinct "gave up" pattern, sends `#ALERT stale_giveup`, and then goes
**quiet** until fresh data arrives. The reasoning: a ring buzzing every few
minutes in a drawer for eight hours trains you to ignore it, and an ignored
alert is a worse hazard than the silence the repeats were guarding against.
The giving-up signal is distinct precisely so silence afterwards reads as
"it stopped trying", not as "it got fixed".

`!SET STALEREPEATS 0` restores unlimited repeating if you want the old
behaviour.

**A watchdog reset buzzes.** If the firmware hangs and the watchdog restarts
it, the ring buzzes the fault pattern at boot as well as reporting
`#BOOT ... wdt_reset=1`. BLE alone only reaches you if a phone happens to be
connected and looking; the motor reaches you regardless.

---

## 8. Upload (once hardware arrives)

```bash
arduino-cli board list
```

```bash
cd ~/projects/glucose-ring && PATH="$PWD/tools/bin:$PATH" LC_ALL=C.UTF-8 LANG=C.UTF-8 arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn Seeeduino:nrf52:xiaonRF52840 .
```

`use_1200bps_touch=true`, so the board enters DFU automatically. If the port
can't be found, **double-tap reset** — a `XIAO-SENSE` volume appears.

Upload uses serial DFU via the generated `.zip`. arduino-cli emits no `.uf2`
for this board (the core's converter skips everything but the T1000-E).

---

## 9. Bring-up checklist

Test in this order. Each step assumes the previous one passed. **Do not skip
to wearing it.**

**Before power**

1. Continuity-check MOSFET source→GND, and confirm the 10k gate pulldown is
   present. With the ring unpowered, D10→GND should read ~10k.
2. **Check the flyback diode orientation** — band to `BAT+`. Backwards is a
   short across the cell.
3. Confirm the motor is on `BAT+`, not `3V3`.

**First power, USB only, no battery**

4. Flash with `-DTEST_MODE=1 -DDEBUG_SERIAL=1 -DWATCHDOG_ENABLE=0`.
5. Watch the boot pattern cycle: tick, low, high, stale, battery. If the
   motor never moves, the fault is the gate drive, not the firmware.
6. **HARD GATE — charge-detect polarity.** On serial, confirm `chg=1` while
   USB is attached and `chg=0` once unplugged. **Do not skip this and do not
   proceed past it.** `~CHG` polarity is assumed, not verified against the
   core (REPORT.md A3). If it is inverted, the guard fails *open* — the ring
   samples the divider while the charger drives the BAT node, which is
   precisely the P0.31 burnout the guard exists to prevent. A guard that
   reads backwards is worse than no guard, because you will trust it.
   If `chg` does not track USB, stop and fix the polarity before attaching a
   cell.

**Battery**

7. Attach the cell. Measure it with a multimeter.
8. Read the reported mV on serial and **calibrate `VBAT_DIVIDER_COMP`** until
   they agree. Do this before trusting the low-voltage cutoff.
9. Unplug USB, confirm `chg=0` and that mV now tracks the cell.
10. Verify battery sampling is skipped while charging: plug USB back in and
    confirm the log says so.

**Idle current — the runtime target lives or dies here**

11. Put a µA meter inline. Measure idle while advertising, and again while
    connected. Compare against §11.
12. Confirm idle current does **not** change between battery samples — that
    verifies the divider is really being released to high-Z.

**BLE**

13. Rebuild with defaults (watchdog on, bonding on), no TEST_MODE.
14. Pair from the phone. Confirm `#BOOT` arrives *after* bonding.
15. Confirm an **unbonded** device is ignored: connect from a second phone,
    write `!BUZZ low`, and verify nothing happens.
16. Walk the protocol: `!VER`, `!GET THRESH`, `!SET THRESH`, `!GET BATT`,
    `!GET STATUS`, `!BUZZ low`.
17. **Power-cycle and confirm config survived** (`!GET THRESH`).

**Behaviour**

18. Push a reading, confirm no response and `!GET STATUS` reflects it.
19. Push the same reading again; confirm it is dropped (timestamp not newer).
20. Push a low reading; confirm `#ALERT` and the buzz.
21. Stop pushing for `STALE_MINUTES`; confirm the stale buzz.
22. Confirm the watchdog: temporarily add an infinite loop, flash, and check
    that it resets and reports `wdt_reset=1`. **Remove it afterwards.**

**Wear test**

23. Run a full charge-to-cutoff cycle and record actual runtime before
    trusting any battery-life estimate.

---

## 10. Layout

```
glucose-ring/
├── glucose-ring.ino     firmware: BLE, motor, ADC, flash, watchdog
├── reading_parser.*     PURE: glucose line parser
├── ring_config.*        PURE: config struct, validation, serialisation
├── command.*            PURE: text command protocol
├── tests/
│   ├── test_all.cpp     host tests for all three pure modules
│   └── run_tests.sh
├── PROTOCOL.md          wire format (for the phone app)
├── PHONE_APP.md         phone app status, toolchain, Nightscout contract
├── PHONE_APP_REPORT.md  phone app: verified, assumed, untested
├── REPORT.md            what is verified, assumed, and unverifiable
├── android/             Kotlin relay app (Nightscout -> BLE NUS -> ring)
│   └── app/src/test/    JVM tests: protocol vectors, Nightscout parsing, staleness
└── tools/bin/python     shim for the core's hardcoded `python`
```

The firmware and the phone app share one repo so PROTOCOL.md sits between
them and both ends are tested against the same vectors.

Arduino compiles top-level `.cpp` files; `tests/` and `tools/` are ignored by
the build.

---

## 11. Power

No LEDs, `BANDWIDTH_LOW`, 200–400 ms connection interval with slave latency
4, PWM released between buzzes, battery divider pulsed at ~0.01% duty, 1 s
FreeRTOS tick idling in WFI.

**On the 40 mAh cell: measure before you trust it.** A realistic idle figure
for a XIAO nRF52840 with a live BLE connection is roughly 50–150 µA, which on
40 mAh spans about one week to about a month. Buzzing moves that materially.
Step 11 of the checklist is not optional if you care about the number.

Levers: raise `CONN_INTERVAL_MAX_MS` (watch the Apple constraint in the
sketch comment), raise `CONN_SLAVE_LATENCY`, lower `TX_POWER_DBM`, lengthen
`TICK_MS`, shorten the patterns, or lower `intensity`.
