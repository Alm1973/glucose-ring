# Phone app — iOS relay: design and status

**Status: WRITTEN, NOT YET BUILT FOR iOS** (2026-09-12). The source is in
`ios/`. Its pure core passes its tests, built with `swiftc`, and the whole app
typechecks against the Mac Catalyst SDK. It has never been compiled for iOS,
opened in Xcode, or run on a phone, because Xcode is not installed. The phone is an iPhone. The
Android app was retired on branch `ios-relay`; its code is in git history
(`nightscout-relay`, app commit `c306487`), and its report is
[PHONE_APP_REPORT.md](PHONE_APP_REPORT.md):

```bash
cd ~/projects/glucose-ring && git restore --source=nightscout-relay -- android/
```

The requirements below carried over from the Android design. Only the
platform, and therefore the architecture, changed. Progress, assumptions and
battery analysis: [IOS_APP_REPORT.md](IOS_APP_REPORT.md).

---

## 1. Architecture: the ring drives the cycle

iOS does not let an app poll a network endpoint on a timer in the background.
It *does* wake a `bluetooth-central` app when a connected peripheral sends a
notification. So the 60 s phone-side poll of the Android design is inverted:

```
ring, every ~5 min (aligned to the last reading's own timestamp)
  -> "#REQ" notify on NUS TX
     -> iOS wakes the app (a few seconds of execution)
        -> app writes "!TIME <now_ms>" at once          (proof of life + trusted clock)
        -> app GETs Nightscout, one request, short timeout
        -> app writes "<sgv>,<dir>,<epoch_ms>" if Nightscout returned one
  -> ring judges the reading by its OWN timestamp against trusted time,
     then buzzes if a threshold is crossed

no "!TIME" within 30 s          -> one miss
3 consecutive misses (~15 min)  -> MISS alarm: a fixed pattern unlike high or low
```

Two alarms that mean different things:

| Ring alarm | Meaning | Most likely cause |
|---|---|---|
| **MISS** | the phone did not answer at all | app killed or force-quit, out of range, Bluetooth off, 7-day signing expired |
| **STALE** | the phone answered, but no current reading | Dexcom/Share outage, sensor warm-up, Nightscout down, **token revoked** |

The app sends `!TIME` *before* its network request, so a slow or failed fetch
never turns into a miss. A revoked token therefore shows as STALE on the ring
and as "token rejected" in the app, not as a dead phone.

Wire details (protocol 3): [PROTOCOL.md](PROTOCOL.md), once the firmware
phase lands.

---

## 2. Requirements carried over (unchanged)

- **Never act on stale data.** A reading older than 15 min is never treated
  as current. The ring now enforces this itself, with trusted time; the app
  also refuses to send stale readings (defence in depth).
- **Never fabricate or interpolate.** The only reading ever sent is the newest
  one Nightscout returned, unchanged except for direction sanitising.
- **Refuse future-dated readings** (beyond 2 min of skew). The ring accepts
  only strictly increasing timestamps, so one bad timestamp would lock out
  every real reading.
- **Strict Nightscout parsing**, as verified against the live instance:
  - `sgv` must be a whole number (`"204"` and `204.5` are refused);
  - `date` is epoch **ms** ≥ `1600000000000`;
  - `type` must be `sgv` if present;
  - `direction` is optional.

  Swift's `Codable` coerces too readily, so decode by hand.
- **Dedupe on `date`.** The ring also drops non-increasing timestamps, and a
  duplicate never refreshes its staleness.
- **Wire format** from `RingProtocol` rules: direction stripped of commas and
  non-printables, max 23 chars, `\n`-terminated, chunked to the maximum write
  length. A write is **not** delivery; confirm with `!GET STATUS`.
- **Token:** read-only subject token, sent as `?token=` (verified). **Never
  logged.** In Swift, `URLError` carries the failing URL, token included, in
  its `userInfo`, so error objects are mapped to fixed text and never logged.
- **Honest status.** The app never looks healthy while relaying nothing.
  "Token rejected" (HTTP 401/403) is a **permanent** state, shown distinctly
  from a transient network failure.
- **Non-goals:**
  - no threshold or alarm logic in the app (that's the firmware's job);
  - no writes to Nightscout;
  - no insulin or pump anything;
  - no analytics, crash reporting or third-party SDKs.

---

## 3. iOS specifics

- **`CBCentralManager` with state restoration**
  (`CBCentralManagerOptionRestoreIdentifierKey`), **and**
  `UIBackgroundModes = bluetooth-central` in `Info.plist`. Without both, iOS
  will not relaunch the app after the system kills it, and the design fails
  silently. `NSBluetoothAlwaysUsageDescription` is also required.
- **`centralManager(_:willRestoreState:)`** reattaches to the peripherals in
  `CBCentralManagerRestoredStatePeripheralsKey`: set the delegate, then
  re-discover and re-subscribe as needed. **Never rescan** on restore.
- **Choosing the ring:** scan once for the NUS service and the name
  `GlucoseRing`, then remember the peripheral **identifier** (iOS exposes no
  MAC address).
- **Reconnect:** on disconnect, call `connect(peripheral)` again at once. On
  iOS a pending connect never expires and is serviced by the system while the
  app is suspended; an app-side timer could not run then anyway. Backoff
  applies only to a connect-then-immediately-fail loop.
- **Pairing:** iOS has no app API to start bonding. The ring sends a Security
  Request on connect, and iOS is expected to answer it with the system pairing
  dialog. **Unverified; this is the top iOS risk** (IOS_APP_REPORT.md).
- **Each wake has a budget of a few seconds:** `!TIME` first, then one GET
  with a ~5 s timeout, then the write-back. No retry loop that outlives the
  window; the next `#REQ` is the retry.
- **Known gaps the ring's MISS alarm exists to catch:**
  - after you **force-quit** the app from the app switcher, iOS does not
    relaunch it for Bluetooth events;
  - after a reboot, not until the app is opened (assumed);
  - with the **free Personal Team** signing you chose, the app stops launching
    **every 7 days** until reinstalled from Xcode.

---

## 4. Secrets

`ios/GlucoseRing/Secrets.plist` holds `NightscoutURL` and `NightscoutToken`.
It is gitignored; `git check-ignore` was confirmed before anything was written
to it. The token is the placeholder `PASTE_READ_ONLY_TOKEN_HERE` until you
paste yours.

- The file is bundled into the app at build time, so, as on Android, **the
  token is inside the built app**.
- A fresh clone has no `Secrets.plist`; the app then says "Not configured".

---

## 5. Toolchain

- **Xcode is required and is not installed.** There's only Command Line Tools
  14.3.1 (Swift 5.8.1, no iOS SDK). You install Xcode yourself; it needs your
  Apple ID sign-in and license acceptance.
- **Target phone: iPhone 11 on iOS 26.6.2.** That needs **Xcode 26**. The
  deployment target is iOS 26.0.
- **Disk: 9.9 GiB free before cleanup.** Plan for roughly 20–25 GB free
  before installing Xcode (estimate).
- **Signing: free Personal Team** (your choice). Builds expire after 7 days.

### Building, once Xcode is installed

1. Paste the token into `ios/GlucoseRing/Secrets.plist`.
2. Open `ios/GlucoseRing.xcodeproj`. **It was written by hand and has never
   been opened in Xcode**, so expect to fix small things.
3. Target → Signing & Capabilities: choose your Personal Team. If the bundle
   ID `com.glucosering.relay` is taken, change it to something unique.
4. On the iPhone: Developer Mode on, trust this Mac. Then Run.
5. Reinstall at least every 7 days (Personal Team). When the app stops
   launching, the ring buzzes MISS.

Without Xcode, `ios/run_core_tests.sh` builds and tests the pure core, and
`tests/run_wire_crosscheck.sh` checks its wire output against the firmware
parser.

---

## 6. Acceptance tests (need the ring and the iPhone)

1. Pairing completes from the ring's Security Request; `#BOOT` arrives.
2. With the app in the background and the screen locked, a `#REQ` wakes it;
   `!TIME` and the reading arrive within the window, and `!GET STATUS` shows
   the reading.
3. System kill: iOS relaunches the app through state restoration on the next
   `#REQ`.
4. Force-quit the app → about 15 min later the ring buzzes **MISS**.
5. Airplane mode on the phone → MISS. Nightscout unreachable while the phone
   is up → STALE after the stale window; the app shows "can't reach
   Nightscout".
6. Revoke the token → the app shows "token rejected" persistently; the ring
   goes STALE, not MISS.
7. A reading older than 15 min is never acted on by the ring, even as the
   first reading after a ring reboot.
8. Leave it overnight: pokes continue, and note the phone battery use.
