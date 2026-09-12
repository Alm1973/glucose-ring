# Phone app — build specification and status

**Status: BUILT — Nightscout relay, v0.2.0 (2026-09-12).** The app polls
Nightscout and relays each new, current reading to the ring. The earlier
xDrip+ broadcast input was removed (it remains in git history, `d1e58ae`).
It compiles — `app-debug.apk`, minSdk 26 / targetSdk 34 — and **76 JVM unit
tests pass**. Its wire output was also run through the firmware's real
`rp_parse()`: 27,888 lines, every one decoded exactly. Details, assumptions and
what is untested: [PHONE_APP_REPORT.md](PHONE_APP_REPORT.md).

Nothing has been installed on a phone or run against real hardware.

## Toolchain actually installed

| Component | Version |
|---|---|
| `android-commandlinetools` (Homebrew cask) | SDK root `/opt/homebrew/share/android-commandlinetools` |
| SDK platform | android-34 |
| Build tools | 34.0.0 |
| Gradle (wrapper, pinned) | 8.11.1 |
| Android Gradle Plugin | 8.7.3 |
| Kotlin | 2.0.21 |
| JDK used for the build | 21.0.4 |

Homebrew's `gradle` is 9.7.1 on JDK 26, and **no AGP supports either**. The
project therefore pins a Gradle 8.11.1 wrapper and points
`org.gradle.java.home` at the JDK 21 already on the machine
(`android/gradle.properties`). Build with the wrapper, not with `gradle`.

```bash
cd ~/projects/glucose-ring/android && ANDROID_HOME=/opt/homebrew/share/android-commandlinetools ./gradlew :app:assembleDebug :app:testDebugUnitTest
```

`android/local.properties` is gitignored and holds `sdk.dir`, `nightscout.url`
and `nightscout.token`. The last two are compiled into `BuildConfig`, so **the
token is inside the APK** (accepted for a sideloaded personal build). Recreate
the file on another machine. `org.json:json` is a **test-only** dependency:
Android's own org.json is a stub under JVM tests, and the APK uses the
framework's.

---

## 1. What the app does

```
Nightscout  --(HTTPS GET every 60 s)-->  this app  --(BLE NUS write)-->  ring
```

It is a courier. It holds no CGM logic, no alerting logic, and no thresholds
— thresholds live on the ring, in flash. Its job: fetch the newest reading,
refuse it unless it is current, reformat it, deliver it, confirm delivery,
keep the link up — and say plainly in its notification whenever any of that
is not happening. It never writes to Nightscout.

---

## 2. Manifest permissions

```xml
<uses-permission android:name="android.permission.INTERNET" />

<!-- Android 12 (API 31)+ -->
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
<uses-permission android:name="android.permission.BLUETOOTH_SCAN"
                 android:usesPermissionFlags="neverForLocation"
                 tools:targetApi="s" />

<!-- Android 11 (API 30) and below -->
<uses-permission android:name="android.permission.BLUETOOTH"
                 android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.BLUETOOTH_ADMIN"
                 android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION"
                 android:maxSdkVersion="30" />

<uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
<uses-permission android:name="android.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE" />
<uses-permission android:name="android.permission.CHANGE_NETWORK_STATE" />
<uses-permission android:name="android.permission.POST_NOTIFICATIONS" />
<uses-permission android:name="android.permission.RECEIVE_BOOT_COMPLETED" />
<uses-permission android:name="android.permission.WAKE_LOCK" />
<uses-permission android:name="android.permission.REQUEST_IGNORE_BATTERY_OPTIMIZATIONS" />
```

```xml
<service
    android:name=".RingService"
    android:foregroundServiceType="connectedDevice"
    android:exported="false" />
```

Notes that bite:

- `BLUETOOTH_CONNECT` and `BLUETOOTH_SCAN` are **runtime** permissions on
  API 31+. Request them before touching any BLE API or you get a
  `SecurityException`. Every BLE call in the app is behind a permission check;
  a refusal shows as "no Bluetooth permission", never as a crash.
- `neverForLocation` on the scan permission avoids needing location on 12+.
  If you ever drop that flag you are back to requesting fine location.
- `POST_NOTIFICATIONS` is runtime on API 33+. Without it the foreground
  service notification is silently suppressed, which makes the service look
  like it died. MainActivity says so if it is refused.
- `foregroundServiceType="connectedDevice"` is **mandatory** on API 34+ for a
  BLE-holding service; omitting it throws at `startForeground()`.
- On API 34 a `connectedDevice` service must also hold one of a list of
  permissions at `startForeground()`. `BLUETOOTH_CONNECT` qualifies but can be
  refused; `CHANGE_NETWORK_STATE` qualifies and is install-time, so a refused
  Bluetooth permission cannot crash the service.

---

## 3. Nightscout contract

Verified against the live instance on 2026-09-12 (PHONE_APP_REPORT.md).

```
GET https://<nightscout.url>/api/v1/entries/sgv.json?count=1&token=<nightscout.token>
```

- Auth: a subject token with role `readable`, sent as `?token=`. Anonymous
  reads return 401 on this instance.
- Response: a JSON array with at most one entry. The app uses three fields:

| Field | Type | Rule in the app |
|---|---|---|
| `sgv` | number | must be a whole number; `"204"` or `204.5` is refused, never coerced |
| `date` | number | epoch **ms**, ≥ `1600000000000`; a seconds value is refused |
| `direction` | string | optional; absent or `null` becomes empty (the ring accepts empty) |

`type`, if present, must be `sgv`. Everything else (`_id`, `sysTime`,
`dateString`, `trend`, `mills`, …) is ignored.

This instance reports `device: nightscout-connect`: Nightscout pulls from the
CGM vendor's cloud, so readings are already minutes old when they land.

The token must never be logged. `NightscoutClient` does not log, and maps
every exception to fixed text, because exception messages (e.g.
`FileNotFoundException` on a 4xx) can contain the full URL.

---

## 4. Relay rules — `Relay.kt`, pure and unit-tested

Staleness is a correctness rule, not a display nicety.

- **Only a current reading is ever sent**: `now − date ≤ 15 min` (the ring's
  default stale window). The ring cannot always catch an old reading by
  itself: after a reboot it learns its clock offset from the first reading it
  receives, so a 40-minute-old first reading looks fresh to it
  (`glucose-ring.ino:632-636, 706-718`). The app is the only guard for that.
- **Dedupe on `date`**, per ring link: each reading is written at most once
  per confirmed link. After a reconnect the current reading may be written
  once more, since the ring may have rebooted; the ring drops a duplicate
  without refreshing its staleness.
- **Future-dated readings are refused** (beyond 2 min of skew). The ring only
  accepts strictly increasing timestamps, so one would lock out every real
  reading until the clock caught up.
- Values outside 20–600 (e.g. sensor-state codes) are never sent.
- **Never fabricated or interpolated**: the only reading ever sent is the
  newest one Nightscout returned, unchanged except for direction sanitising.

The wire line comes from `RingProtocol.formatReading()`: direction stripped of
commas and non-printables, max 23 chars, `\n`-terminated, no NUL on the wire.

### What the notification says

The notification is the only affordance for "is this working?". Its title is
`Relaying 204 FortyFiveDown` **only** when a fresh reading was written and the
ring's own `#STATUS` reports that sgv with `stale=0`. Otherwise it reads
`Sending … to the ring` while waiting for that confirmation, or
`NOT relaying: <reason>` with a plain-language explanation:

| Condition | Title reason |
|---|---|
| token or URL not set in local.properties | not configured |
| HTTP 401 / 403 | token rejected |
| other HTTP status | Nightscout error |
| no network, DNS, TLS, timeout | can't reach Nightscout |
| body not understood | unexpected reply |
| newest reading older than 15 min | no current glucose data |
| empty result | no readings in Nightscout |
| reading dated in the future | reading dated in the future |
| value outside 20–600 | not a usable reading |
| no poll completed for 5 min | Nightscout not checked |
| Bluetooth permission refused / Bluetooth off / no ring chosen | as stated |
| ring not connected / not paired / not answering | as stated |
| ring's `#STATUS` disagrees | ring did not take the reading |

The channel is `IMPORTANCE_LOW`, i.e. silent. It is a status display, not an
alarm (§7): alarms belong to the ring and to the phone's own CGM app.

`RelayTest.relayingOnlyForFreshDataConfirmedByTheRing` checks exhaustively
that "Relaying" appears for exactly one combination of Nightscout result ×
link state × ring reply.

---

## 5. BLE client flow — `NusClient.kt`

UUIDs (PROTOCOL.md, cross-checked against the core's `BLEUart.cpp:45-60`):

```kotlin
val NUS_SERVICE = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
val NUS_RX      = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")  // we write
val NUS_TX      = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")  // we subscribe
val CCCD        = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")
```

Order:

1. **Choose the ring.** MainActivity scans filtered on `NUS_SERVICE` and
   requires the name `GlucoseRing`, which the ring sends in its scan response
   (`glucose-ring.ino:856`). The MAC is saved; the service connects to it
   directly from then on.
2. **Connect** — `connectGatt(ctx, autoConnect = false, callback, TRANSPORT_LE)`.
3. **Bond.** If not bonded, `createBond()` and wait for
   `ACTION_BOND_STATE_CHANGED` → `BOND_BONDED` (60 s timeout; the user may
   have to accept a dialog).
4. **Request MTU** 247, then **discover services** — after bonding, not
   before. Discovering across a bonding transition can return a stale table.
5. **Subscribe to TX**: `setCharacteristicNotification(tx, true)` **and** the
   CCCD write. Both are required; the first alone does nothing on the wire.
6. **Handshake**: send `!VER` up to 3 times, 10 s apart, until `#BOOT` or
   `#VER` arrives. Only then is the link `CONFIRMED` and readings written. A
   `!VER` sent before encryption is discarded by the ring, hence the retries.
7. After each reading, send `!GET STATUS`. The reply is what turns the
   notification to "Relaying".

> A successful `writeCharacteristic` **is not delivery.** If the link is not
> yet encrypted the ring discards the bytes and does not queue them.

**Serialise every GATT operation.** Android's GATT stack allows exactly one
outstanding operation per connection; issuing a second write before
`onCharacteristicWrite` fires drops it silently. Writes go through a queue.
All GATT callbacks are posted to the main thread, so link state is only ever
touched from one thread. A failed with-response write drops the link; the
reading is resent after the next confirmed connection if it is still current.

---

## 6. Chunking and reassembly

The ring reassembles by newline, so a message may span writes — but the
client must still respect the negotiated MTU: usable payload = MTU − 3.

At the default 23-byte MTU the payload is 20 bytes, and a typical reading is
32 (`204,FortyFiveDown,1789184103083\n`), so **chunking is the normal case,
not an edge case**.

Inbound, the ring's replies also arrive in notification-sized pieces.
`RingProtocol.LineAssembler` accumulates and splits on `\n`, and discards its
buffer past 512 bytes so a wedged link cannot grow it without bound.

---

## 7. Reconnect strategy

The ring re-advertises forever on disconnect (`restartOnDisconnect(true)`,
`Advertising.start(0)`), so reconnection is always possible.

- On disconnect: **always `gatt.close()`** before retrying. Leaking GATT
  clients exhausts a small system-wide pool and eventually no app on the phone
  can connect.
- Reconnect with exponential backoff — 1 s, 2 s, 4 s … capped at 60 s — with
  jitter (each delay is 50–100 % of its step). Do not hot-loop; it drains both
  batteries.
- Reset backoff only after a *confirmed* link (`#BOOT` or `#VER` received), not
  merely on `STATE_CONNECTED`.
- One mechanism only: `autoConnect = false` plus the app's own backoff. The
  previous version combined `autoConnect = true` with an app timer, and each
  cancelled the other.
- Status `133` (`GATT_ERROR`) is Android's catch-all. Treat it as a normal
  retry case, not a bug.
- Every connection runs the full sequence in §5, so notifications are
  re-subscribed after every reconnect.

**Do not implement app-side alert logic on disconnect.** Staleness is the
ring's job — it buzzes on its own after `STALE_MINUTES` with no fresh data.
Duplicating that in the app produces two uncoordinated alarms. The app *does*
report staleness, in its silent notification text, never as sound or
vibration.

---

## 8. Foreground service

- `startForeground()` runs immediately in `onCreate`, well inside the
  5-second window, or the system kills the service.
- The notification shows the relay status of §4 — it is the only affordance
  the user has for "is this working?".
- `START_STICKY`, plus a `BOOT_COMPLETED` receiver to restart after reboot.
- A **partial wake lock** is held for the service's lifetime. Handler timers
  do not run while the CPU sleeps, and the 60 s poll must keep running with
  the screen off. This costs battery; the cost is unmeasured.
- MainActivity's "Allow running in background" button requests the
  battery-optimisation exemption (`ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`).
  Without it, aggressive OEM power management (Samsung, Xiaomi, OnePlus
  especially) will kill the service and the ring will go quiet with no
  indication.

> This is the weakest link in the whole system: an Android OEM silently
> killing the courier service. It is also exactly why the ring alerts on
> staleness by itself, and why the phone's own CGM alarms must stay on.

---

## 9. Config UI (optional, second pass — not built)

Everything in PROTOCOL.md §4 is available over the same link, so the app can
expose: thresholds (`!GET`/`!SET THRESH`), stale window, repeat interval,
intensity, buzz patterns, `!BUZZ` for a test, and `!GET BATT` / `!GET STATUS`.

Battery is also on the standard **Battery Service `0x180F`**, so a generic
battery widget works without any custom command.

`#BOOT ... wdt_reset=1` is already surfaced in the notification text.

---

## 10. Acceptance tests

Automated (JVM, `./gradlew :app:testDebugUnitTest`):

1. Formatter against PROTOCOL.md §8 vectors — `RingProtocolTest`.
2. A direction containing a comma is sanitised, not sent — `RingProtocolTest`,
   `NightscoutTest.hostileDirectionCannotBreakTheWireLine`.
3. A duplicate/older timestamp is skipped client-side — `RelayTest`.
4. Nothing older than 15 min, future-dated, or outside 20–600 is ever sent —
   `RelayTest`.
5. The live Nightscout body parses to the documented wire line, and every
   truncation of it is rejected — `NightscoutTest`.

Host-side cross-check (one-off, PHONE_APP_REPORT.md): the formatter's output,
fed in 20-byte chunks through the firmware's real `rp_parse()`, decodes
exactly.

On a device — **not yet done**:

6. A reading spanning two writes at 23-byte MTU arrives intact (`!GET STATUS`).
7. Writing before bond completes is treated as *not delivered*.
8. Kill the app from recents → confirm the service restarts.
9. Airplane mode on/off → confirm "can't reach Nightscout", backoff reconnect,
   and that backoff resets only after `#BOOT`/`#VER`.
10. Revoke the token → confirm "token rejected".
11. Leave it overnight, screen off → confirm the service is still polling at
    morning, and note the battery cost of the wake lock.

---

## 11. Module layout

```
app/src/main/java/com/glucosering/ring/
├── RingService.kt        foreground service: 60 s poll, wake lock, notification
├── Relay.kt              PURE: staleness, dedupe, status text, reconnect backoff
├── Nightscout.kt         PURE: response parsing (org.json only)
├── NightscoutClient.kt   HTTPS GET, token-safe errors
├── NusClient.kt          connect, bond, MTU, subscribe, handshake, write queue
├── RingProtocol.kt       PURE: wire format, command builders, #response parser
├── MainActivity.kt       permissions, Find ring, battery exemption, status
└── BootReceiver.kt       restart after reboot

app/src/test/java/com/glucosering/ring/
├── RingProtocolTest.kt   PROTOCOL.md section 8 vectors
├── NightscoutTest.kt     live body, wrong types, every truncation
├── RelayTest.kt          staleness, dedupe, status, exhaustive "Relaying" check
└── WireExportTest.kt     dumps wire bytes for the host-side rp_parse() check
```

The pure files have no Android imports, so they are unit-testable on the JVM —
the same split that makes the firmware's parser testable on the host.
