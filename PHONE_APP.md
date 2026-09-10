# Phone app — build specification

**Status: BLOCKED.** Nothing was scaffolded or compiled. This document
specifies what needs building.

---

## 0. Why blocked

Probed on this machine:

| Tool | Result |
|---|---|
| `java` / `javac` | **present** — OpenJDK 22.0.2 |
| `sdkmanager` | not found |
| `adb` | not found |
| `gradle` | not found |
| `kotlinc` | not found |
| Android Studio | not installed |
| `ANDROID_HOME` / `ANDROID_SDK_ROOT` | unset |
| `~/Library/Android/sdk`, `~/Android/Sdk`, `/usr/local/share/android-sdk`, `/opt/homebrew/share/android-sdk` | all absent |

A JDK on its own cannot build an Android app. It needs the Android SDK
(`android.jar`, platform, build-tools), Gradle, and the Android Gradle
Plugin. Even a Gradle *wrapper* downloads Gradle on first run, and AGP pulls
the SDK and requires accepting SDK licences — all of which is installing
software. You said not to install anything, so I stopped here.

**To unblock**, either:

```
brew install --cask android-studio
```

then open it once to install an SDK platform and accept licences; or install
just the command-line tools and set `ANDROID_HOME`. Tell me when that's done
and I'll scaffold and compile.

---

## 1. What the app does

```
xDrip+  --(local broadcast intent)-->  this app  --(BLE NUS write)-->  ring
```

It is a courier. It holds no CGM logic, no alerting logic, and no thresholds
— thresholds live on the ring, in flash. The app's job is: receive a reading,
reformat it, deliver it, and keep the link up.

---

## 2. Manifest permissions

```xml
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
<uses-permission android:name="android.permission.POST_NOTIFICATIONS" />
<uses-permission android:name="android.permission.RECEIVE_BOOT_COMPLETED" />
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
  `SecurityException`.
- `neverForLocation` on the scan permission avoids needing location on 12+.
  If you ever drop that flag you are back to requesting fine location.
- `POST_NOTIFICATIONS` is runtime on API 33+. Without it the foreground
  service notification is silently suppressed, which makes the service look
  like it died.
- `foregroundServiceType="connectedDevice"` is **mandatory** on API 34+ for a
  BLE-holding service; omitting it throws at `startForeground()`.

---

## 3. xDrip+ intent contract

> **UNVERIFIED.** There is no xDrip+ source or APK on this machine, so I could
> not check these strings against anything. They are from knowledge of the
> xDrip+ codebase, not from inspection. **Confirm against xDrip+'s
> `Intents.java` before trusting them**, and log the full extras bundle on
> first run to see what actually arrives. If a name is wrong the receiver
> silently never fires, which is the worst failure mode — so verify first.

In xDrip+: **Settings → Inter-app settings → "Broadcast locally"** must be on.

Action:

```
com.eveningoutpost.dexdrip.BgEstimate
```

Extras:

| Key | Type | Meaning |
|---|---|---|
| `com.eveningoutpost.dexdrip.Extras.BgEstimate` | `double` | mg/dL, **always mg/dL regardless of display units** |
| `com.eveningoutpost.dexdrip.Extras.Time` | `long` | epoch **milliseconds** |
| `com.eveningoutpost.dexdrip.Extras.BgSlopeName` | `String` | e.g. `Flat`, `FortyFiveUp`, `NOT COMPUTABLE` |
| `com.eveningoutpost.dexdrip.Extras.Raw` | `double` | raw value |
| `com.eveningoutpost.dexdrip.Extras.Source` | `String` | source description |

Register the receiver **at runtime** in the service (`registerReceiver`), not
in the manifest — implicit broadcasts from other apps to manifest-declared
receivers are restricted from API 26 onward.

```kotlin
private val bgReceiver = object : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        val mgdl = intent.getDoubleExtra("com.eveningoutpost.dexdrip.Extras.BgEstimate", 0.0)
        val time = intent.getLongExtra("com.eveningoutpost.dexdrip.Extras.Time", 0L)
        val slope = intent.getStringExtra("com.eveningoutpost.dexdrip.Extras.BgSlopeName") ?: ""
        onReading(mgdl, time, slope)
    }
}
```

---

## 4. Formatting a reading

Must match [PROTOCOL.md](PROTOCOL.md) exactly. The ring drops anything
malformed **silently and without buzzing**, so a formatting bug looks
identical to a dead link.

```kotlin
/** Returns the wire line, or null if this reading must not be sent. */
fun formatReading(mgdl: Double, epochMs: Long, slopeName: String): String? {
    val sgv = Math.round(mgdl).toInt()
    if (sgv < 20 || sgv > 600) return null          // ring rejects; don't bother
    if (epochMs < 1_600_000_000_000L) return null   // ring rejects: epoch floor

    // CRITICAL: a comma in the direction would create a fourth field and the
    // ring would reject the whole line. Also strip anything non-printable and
    // cap at 23 chars -- 24+ is an error on the ring, not a truncation.
    val dir = slopeName
        .filter { it.code in 0x20..0x7E && it != ',' }
        .take(23)

    return "$sgv,$dir,$epochMs\n"
}
```

**Strictly increasing timestamps.** The ring drops any reading whose
`epoch_ms` is not greater than the last accepted one, and a duplicate does
**not** refresh staleness. Keep the last sent timestamp in the service and
skip anything not newer:

```kotlin
private var lastSentEpoch = 0L
fun shouldSend(epochMs: Long) = epochMs > lastSentEpoch
```

xDrip+ can re-broadcast the same reading (backfill, calibration, app resume),
so this check is not optional.

---

## 5. BLE client flow

UUIDs (from PROTOCOL.md):

```kotlin
val NUS_SERVICE = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
val NUS_RX      = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")  // we write
val NUS_TX      = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")  // we subscribe
val CCCD        = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")
val BATTERY_SVC = UUID.fromString("0000180F-0000-1000-8000-00805F9B34FB")
```

Order matters:

1. **Scan** filtered on `NUS_SERVICE`, or connect directly to a remembered MAC.
2. **Connect** — `connectGatt(ctx, autoConnect = false, callback, TRANSPORT_LE)`.
   Use `autoConnect = true` only for background reconnect (§7).
3. **Bond.** The ring requests pairing on connect and **discards every byte
   from an unencrypted link.** Wait for `BluetoothDevice.ACTION_BOND_STATE_CHANGED`
   to reach `BOND_BONDED` before writing anything.
4. **Discover services** — after bonding, not before. Discovering across a
   bonding transition can return a stale service table.
5. **Request MTU** — `requestMtu(247)`. Do not assume it is granted; chunk
   regardless (§6).
6. **Subscribe to TX**: `setCharacteristicNotification(tx, true)` **and** write
   `ENABLE_NOTIFICATION_VALUE` to the CCCD descriptor. Both are required;
   the first alone does nothing on the wire.
7. **Wait for `#BOOT`**, or send `!VER` and wait for `#VER`. Only then treat
   the link as usable.
8. Push readings as they arrive.

> A successful `writeCharacteristic` **is not delivery.** If the link is not
> yet encrypted the ring discards the bytes and does not queue them. Confirm
> with `!GET STATUS` if you need certainty.

**Serialise every GATT operation.** Android's GATT stack allows exactly one
outstanding operation per connection; issuing a second write before
`onCharacteristicWrite` fires drops it silently. Use a queue.

---

## 6. Chunking and reassembly

The ring reassembles by newline, so a message may span writes — but you must
still respect the negotiated MTU.

```kotlin
// usable payload = MTU - 3 (ATT opcode + handle)
private fun writeLine(line: String) {
    val bytes = line.toByteArray(Charsets.US_ASCII)
    val chunk = (negotiatedMtu - 3).coerceAtLeast(20)
    var off = 0
    while (off < bytes.size) {
        val n = minOf(chunk, bytes.size - off)
        enqueueWrite(bytes.copyOfRange(off, off + n))
        off += n
    }
}
```

At the default 23-byte MTU the payload is 20 bytes and a typical reading is
23, so **chunking is the normal case, not an edge case**.

Inbound, the ring's replies also arrive in notification-sized pieces.
Accumulate into a buffer and split on `\n`:

```kotlin
private val rxBuf = StringBuilder()
fun onNotify(data: ByteArray) {
    rxBuf.append(String(data, Charsets.US_ASCII))
    while (true) {
        val i = rxBuf.indexOf("\n")
        if (i < 0) break
        val line = rxBuf.substring(0, i).trimEnd('\r')
        rxBuf.delete(0, i + 1)
        if (line.isNotEmpty()) onRingLine(line)
    }
}
```

Cap `rxBuf` (say 512 bytes) and discard on overflow, so a wedged link cannot
grow it without bound.

---

## 7. Reconnect strategy

The ring re-advertises forever on disconnect (`restartOnDisconnect(true)`,
`Advertising.start(0)`), so reconnection is always possible.

- On `onConnectionStateChange` → `STATE_DISCONNECTED`: **always call
  `gatt.close()`** before retrying. Leaking GATT clients exhausts a small
  system-wide pool and eventually no app on the phone can connect.
- Reconnect with exponential backoff — 1 s, 2 s, 4 s … capped at 60 s — with
  jitter. Do not hot-loop; it drains both batteries.
- Reset backoff to 1 s only after a *confirmed* link (`#BOOT` or `#VER`
  received), not merely on `STATE_CONNECTED`.
- For long idle periods prefer `autoConnect = true`, which hands reconnection
  to the system and is far cheaper than app-driven scanning.
- Status `133` (`GATT_ERROR`) is Android's catch-all. Treat it as a normal
  retry case, not a bug.
- Re-subscribe to notifications after every reconnect. Bonding persists;
  CCCD subscriptions do not, unless the ring stored them per-bond.

**Do not implement app-side alert logic on disconnect.** Staleness is the
ring's job — it buzzes on its own after `STALE_MINUTES` with no fresh data.
Duplicating that in the app produces two uncoordinated alarms.

---

## 8. Foreground service

- Start with `startForeground()` immediately in `onCreate`, well inside the
  5-second window, or the system kills it.
- Notification should show link state and last reading — it is the only
  affordance the user has for "is this working?".
- `START_STICKY`, plus a `BOOT_COMPLETED` receiver to restart after reboot.
- Send the user to battery-optimisation exemption
  (`ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`). Without it, aggressive
  OEM power management (Samsung, Xiaomi, OnePlus especially) will kill the
  service and the ring will go quiet with no indication.

> This is the weakest link in the whole system: an Android OEM silently
> killing the courier service. It is also exactly why the ring alerts on
> staleness by itself, and why the phone's own CGM alarms must stay on.

---

## 9. Config UI (optional, second pass)

Everything in PROTOCOL.md §4 is available over the same link, so the app can
expose: thresholds (`!GET`/`!SET THRESH`), stale window, repeat interval,
intensity, buzz patterns, `!BUZZ` for a test, and `!GET BATT` / `!GET STATUS`.

Battery is also on the standard **Battery Service `0x180F`**, so a generic
battery widget works without any custom command.

Surface `#BOOT ... wdt_reset=1` somewhere visible — it means the ring's
watchdog recovered it from a hang, which the wearer should know about.

---

## 10. Acceptance tests

1. Formatter unit tests against PROTOCOL.md §8 vectors — that table is
   already executable in `tests/test_all.cpp`; mirror it in Kotlin.
2. A direction string containing a comma must be sanitised, not sent.
3. A duplicate/older timestamp must be skipped client-side.
4. A reading spanning two writes at 23-byte MTU must arrive intact.
5. Writing before bond completes must be treated as *not delivered*.
6. Kill the app from recents → confirm the service restarts.
7. Airplane mode on/off → confirm backoff reconnect, and that backoff resets
   only after `#BOOT`/`#VER`.
8. Leave it overnight → confirm the service is still alive at morning.

---

## 11. Suggested module layout

```
app/src/main/java/.../
├── RingService.kt        foreground service; owns the GATT client
├── XdripReceiver.kt      broadcast receiver -> Reading
├── Reading.kt            data class + formatReading()
├── NusClient.kt          connect, bond, MTU, write queue, chunking, reassembly
├── RingProtocol.kt       command builders + #response parser
└── MainActivity.kt       permissions, pairing, status, config UI
```

`Reading.kt` and `RingProtocol.kt` should be pure Kotlin with no Android
imports, so they are unit-testable on the JVM — the same split that makes the
firmware's parser testable on the host.
