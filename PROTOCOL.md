# glucose-ring wire protocol

**Protocol version 3** (`proto=3`), firmware `3.0.0`, schema `2`.

**Protocol 3 inverts the cycle for iOS:** the ring asks (`#REQ`), and the
phone answers (`!TIME`, then the reading). See §9. Changes from protocol 2:

- A reading is accepted only after a `!TIME` since boot, and is judged by its
  **own timestamp** against that trusted time, never by when it arrived (§3).
- New: `!TIME`, `!GET LINK`, `#REQ`, `#ALERT miss`, `#ALERT miss_giveup`, the
  alert state `miss`, and the buzz name `miss`.
- Unchanged: UUIDs, framing, the reading format and every parser rule.

Transport: **Nordic UART Service (NUS)**, the same link in both directions.

| | UUID |
|---|---|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (phone **writes** here) | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX (phone **subscribes**, ring notifies) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

Advertised name: `GlucoseRing`. The ring also exposes the standard
**Battery Service `0x180F`**, so battery level is readable without any
custom command.

---

## 1. Framing

- ASCII, **`\n`-terminated**. `\r` is accepted and ignored (so `\r\n` is fine).
- One message per line. A line is processed only when `\n` arrives.
- Max accepted line length: **95 bytes** plus the newline. A longer line is
  discarded and the parser resynchronises at the next `\n` — it does not
  truncate-and-act.
- **A message may be split across several BLE writes**, and several messages
  may share one write. The ring reassembles by newline. This matters: at the
  default 23-byte MTU the usable payload is 20 bytes, and a typical reading
  is 23. Do not assume one write equals one message.
- The ring never sends a message longer than 200 bytes.

## 2. Message types

Direction is unambiguous from the first byte:

| First byte | Meaning | Direction |
|---|---|---|
| `0`–`9` | glucose reading | phone → ring |
| `!` | command | phone → ring |
| `#` | response or notification | ring → phone |

A leading space is **not** trimmed: `" !VER"` is not a command and will be
rejected as a malformed reading.

---

## 3. Glucose reading (phone → ring)

```
<sgv>,<direction>,<epoch_ms>
```

```
112,Flat,1789238400000
```

| Field | Rule |
|---|---|
| `sgv` | Bare decimal digits, **no sign, no decimal point, no whitespace, no `0x`**. Leading zeros allowed (`070` = 70). Must be **20–600** mg/dL. |
| `direction` | 0–23 characters, printable ASCII (`0x20`–`0x7E`) only. **May be empty. May contain spaces** — `NOT COMPUTABLE` and `RATE OUT OF RANGE` are valid. 24+ chars is an error, not a truncation. |
| `epoch_ms` | Bare decimal digits. Unix **milliseconds**, must be ≥ `1600000000000` (≈2020-09-13). Seconds-precision timestamps are rejected. |

Exactly three fields. A fourth field is an error, not ignored.

### Acceptance rules beyond parsing

- **Trusted time is required.** The ring has no clock of its own. A reading is
  dropped unless the phone has sent `!TIME` since the ring booted.
- **Judged by its own timestamp, never by arrival.** Against the phone's
  `!TIME`, a reading whose `epoch_ms` is older than the stale window
  (`!GET STALE`, default 15 min) is dropped. So is one dated more than 2 min in
  the future, because it would lock out every later reading (next rule).
  Protocol 2 inferred the clock from the readings themselves, so after a
  reboot a 40-minute-old first reading looked fresh. That is gone.
- **Timestamps must strictly increase.** A reading whose `epoch_ms` is less
  than or equal to the last accepted one is silently dropped. Re-sending an
  old value does **not** refresh staleness — a phone that is alive but
  relaying old CGM data is still stale data.
- Readings are **not acknowledged.** Nothing is sent back on success. If you
  need confirmation, follow with `!GET STATUS`.
- A malformed reading is dropped silently and never buzzes.

### Rejection reasons (internal, not sent on the wire)

`ERR_EMPTY`, `ERR_FIELD_COUNT`, `ERR_SGV_FORMAT`, `ERR_SGV_RANGE`,
`ERR_DIRECTION`, `ERR_EPOCH_FORMAT`, `ERR_EPOCH_RANGE`.

After parsing (`liveness.h`): `NO_CLOCK`, `STALE`, `FUTURE`.

---

## 4. Commands (phone → ring)

```
!<VERB> [<arg> ...]
```

Arguments are separated by **spaces**; runs of spaces collapse. A token
longer than 39 characters, or more than 8 tokens, is rejected as malformed.
Verbs and keys are **case-sensitive and uppercase**.

Every command produces **exactly one** response line.

| Command | Success response |
|---|---|
| `!VER` | `#VER glucose-ring 3.0.0 proto=3 schema=2` |
| `!GET THRESH` | `#THRESH low=70 high=200` (or `low=unset`) |
| `!SET THRESH <low> <high>` | `#OK SET THRESH` |
| `!GET STALE` | `#STALE 15` |
| `!SET STALE <minutes>` | `#OK SET STALE` |
| `!GET REPEAT` | `#REPEAT 5` |
| `!SET REPEAT <minutes>` | `#OK SET REPEAT` |
| `!GET INTENSITY` | `#INTENSITY 100` |
| `!SET INTENSITY <1-100>` | `#OK SET INTENSITY` |
| `!GET STALEREPEATS` | `#STALEREPEATS 6` |
| `!SET STALEREPEATS <0-255>` | `#OK SET STALEREPEATS` |
| `!GET PATTERN <name>` | `#PATTERN low 400/200,400/0` |
| `!SET PATTERN <name> <spec>` | `#OK SET PATTERN` |
| `!GET BATT` | `#BATT mv=4012 pct=87 chg=0` |
| `!GET STATUS` | see below |
| `!GET LINK` | `#LINK misses=0 clock=1 next_poke_s=212` |
| `!TIME <epoch_ms>` | `#OK TIME` |
| `!BUZZ <name>` | `#OK BUZZ low` |
| `!SAVE` | `#OK SAVE` |
| `!RESET CONFIG` | `#OK RESET CONFIG` |

`<name>` for `PATTERN` is `low`, `high` or `stale`.
`<name>` for `BUZZ` is `low`, `high`, `stale`, `battery`, `tick`, `fault`,
`giveup` or `miss`.

### `!TIME <epoch_ms>` (protocol 3)

The phone's current wall-clock time: Unix **milliseconds**, bare digits,
≥ `1600000000000`. Send it **first** in every answer to `#REQ`, before any
network request. It does two things:

- it sets the ring's trusted clock, against which every reading's own
  timestamp is judged (§3);
- it is the proof of life that clears the miss count (§9). Any valid `!TIME`
  counts, whether or not a poke asked for it.

The ring does not second-guess the value. A wrong phone clock makes readings
look stale or future-dated, so the ring alarms; it never makes old data look
current.

### `!GET LINK`

```
#LINK misses=0 clock=1 next_poke_s=212
```

- `misses`: consecutive unanswered pokes.
- `clock`: `1` once a `!TIME` has arrived since boot.
- `next_poke_s`: seconds until the next `#REQ`.

### Stale repeat cap

`stale_repeats` (default **6**) is how many consecutive stale buzzes the ring
emits before it gives up: it sends `#ALERT stale_giveup`, buzzes a distinct
terminal pattern, and then stays **silent** on staleness until a fresh reading
arrives. Any fresh reading rearms it.

The MISS alarm (§9) shares this budget. After that many MISS buzzes the ring
sends `#ALERT miss_giveup` and goes quiet the same way. A switch between
STALE and MISS rearms the budget, because it says something new.

`!SET STALEREPEATS 0` means never stop repeating.

A client must not read the silence after `stale_giveup` as recovery. The ring
still reports `alert=stale` in `!GET STATUS`; it has merely stopped buzzing.

### `!GET STATUS`

```
#STATUS sgv=112 dir=Flat age_s=42 stale=0 alert=none up_s=3600
```

With no reading yet received:

```
#STATUS sgv=none dir= age_s=0 stale=1 alert=stale up_s=42
```

`alert` is one of `none` `low` `high` `stale` `miss`. `stale` is `1`/`0`;
`1` means there is no current reading by trusted time.
`age_s` is seconds since the reading was **received**, not since its
timestamp.

### `!GET BATT`

```
#BATT mv=4012 pct=87 chg=0
```

`chg` is `1` charging, `0` not charging, `-1` unknown (no sample taken yet).
`mv` and `pct` come from the **last completed sample**, taken at most once a
minute — they are not read on demand. Both read `0` until the first sample.
**While charging, the battery is not sampled at all** (sampling the divider
with the charger driving the node is a documented hazard), so `mv`/`pct` will
be the last pre-charge values and `chg=1`.

### Thresholds

`<low>` and `<high>` are `20`–`600`, or the literal `unset`.
If both are set, `low` **must** be strictly less than `high`.
An unset side never alerts.

### Pattern spec

```
<on_ms>/<off_ms>[,<on_ms>/<off_ms>]...
```

1–6 pulses. `on_ms` is 10–2000, `off_ms` is 0–5000. No trailing comma, no
spaces inside the spec (a space would split it into two tokens).

```
!SET PATTERN low 500/300,500/300,500/0
```

### Persistence

`!SET` applies **immediately in RAM** and is written to flash after **5
seconds of quiet**. This debounce exists so a phone that spams `!SET` cannot
burn through flash erase cycles. `!SAVE` forces an immediate write.

Config survives reboot. `!RESET CONFIG` restores factory defaults —
**including returning both thresholds to `unset`** — and persists.

### Errors

```
#ERR <VERB> <reason>
```

```
#ERR SET low must be below high
#ERR GET unknown key
#ERR ? unknown verb
```

`?` appears as the verb when the command could not be identified at all.
Reason text is human-readable and **may change**; branch on the `#ERR`
prefix and the verb, not on the reason string.

A rejected `!SET` leaves the configuration **completely unchanged** — it is
validated on a scratch copy and only committed if the whole config is still
valid. There is no partial application.

---

## 5. Unsolicited notifications (ring → phone)

Sent without being asked. A client must tolerate these arriving at any time,
including interleaved between a command and its response.

| Message | When |
|---|---|
| `#BOOT ver=3.0.0 proto=3 wdt_reset=0 up_s=3` | once per connection, after the link is encrypted |
| `#REQ` | every ~5 min, and at once when a trusted link subscribes. Answer with `!TIME`, then the reading (§9) |
| `#ALERT low sgv=55` | when the alert state *changes* to low/high/stale/miss |
| `#ALERT stale_giveup` | the stale repeat cap was reached; buzzing stops until fresh data |
| `#ALERT miss_giveup` | the same cap, reached while in MISS |

`wdt_reset=1` means the previous reset was caused by the **watchdog**, i.e.
the firmware hung and was restarted. Surface this — it is the signal that the
ring recovered from a fault rather than being power-cycled normally.

`#ALERT` fires on **transitions only**, not on every repeat buzz.
`#ALERT stale_giveup` is the exception: it fires once when the cap is hit, and
is a *state* message, not a transition — the ring is still stale afterwards.

`wdt_reset=1` is also accompanied by a distinct **fault buzz** at boot, so the
wearer learns about it even with no phone connected.

---

## 6. Pairing and security

The ring requests pairing on connect and **ignores every byte received over
an unencrypted link** — no commands, no readings, no responses, not even
`#BOOT`. Bond first, then talk.

Practically, for the phone app:

1. Connect.
2. Complete pairing/bonding when the OS prompts.
3. Subscribe to TX notifications.
4. Wait for `#BOOT`, or send `!VER` and wait for `#VER`.
5. Answer every `#REQ`: send `!TIME <now_ms>` at once, then the newest reading
   if you have one (§9).

If you write before bonding completes, those writes are **discarded, not
queued**. Do not treat a successful GATT write as delivery.

> The NUS characteristics themselves are advertised with open GATT
> permissions — this core's `BLEUart` hardcodes that and it cannot be
> changed without forking the library. The gate is enforced in firmware
> instead. See REPORT.md for exactly what that does and does not buy.

---

## 7. Worked example

```
ring  -> #BOOT ver=3.0.0 proto=3 wdt_reset=0 up_s=2
ring  -> #REQ                         (link just became usable: ask at once)
phone -> !TIME 1789238460000          (first, before any network request)
ring  -> #OK TIME
phone -> 112,Flat,1789238400000       (1 min old by trusted time: accepted;
                                       no response - readings are not acked)
phone -> !SET THRESH 70 200
ring  -> #OK SET THRESH
         ... ~5 min later ...
ring  -> #REQ
phone -> !TIME 1789238760000
ring  -> #OK TIME
phone -> 65,SingleDown,1789238700000
ring  -> #ALERT low sgv=65
                                      (motor buzzes the low pattern)
phone -> !GET STATUS
ring  -> #STATUS sgv=65 dir=SingleDown age_s=3 stale=0 alert=low up_s=310
phone -> 65,SingleDown,1789238700000
                                      (dropped: timestamp not newer)
         ... the phone goes quiet; #REQ goes unanswered ...
ring  -> #ALERT stale sgv=65          (reading now >15 min old: STALE)
ring  -> #ALERT miss sgv=65           (3rd unanswered #REQ: MISS, 2 x 1000 ms)
```

## 8. Test vectors

Accepted:

```
112,Flat,1789238400000
20,,1600000000000
600,Flat,1600000000000
070,Flat,1789238400000
95,RATE OUT OF RANGE,1789238400000
```

Rejected:

```
112,Flat                          (two fields)
112,Flat,1789238400000,x          (four fields)
-5,Flat,1789238400000             (sign)
112.5,Flat,1789238400000          (decimal)
 112,Flat,1789238400000           (leading space)
19,Flat,1789238400000             (below 20)
601,Flat,1789238400000            (above 600)
112,Flat,1599999999999            (before the epoch floor)
112,Flat,0                        (bad timestamp)
```

Every line above is an executable assertion in `tests/test_all.cpp`. The
acceptance rules of §3 (trusted time, staleness, future-dated) are asserted in
that file's `liveness` sections.

---

## 9. The wake cycle (protocol 3)

iOS will not let an app poll the network on a timer in the background, but it
wakes a `bluetooth-central` app when a connected peripheral sends a
notification. So the ring drives the cycle. The constants are compile-time,
in `liveness.h`:

| Constant | Value | Meaning |
|---|---|---|
| `LV_POKE_INTERVAL_MS` | 5 min | cadence of `#REQ` (Dexcom's) |
| `LV_ANSWER_TIMEOUT_MS` | 30 s | the phone must send `!TIME` within this |
| `LV_MISS_ALERT_COUNT` | 3 | consecutive misses → MISS (~15 min blind) |
| `LV_ALIGN_LAG_MS` | 2 min | *assumed* CGM → Nightscout lag, used for alignment |
| `LV_MIN_POKE_GAP_MS` | 1 min | alignment never pokes sooner than this |
| `LV_FUTURE_TOLERANCE_MS` | 2 min | clock skew allowed before "dated in the future" |

1. `#REQ` goes out when due, and only on a trusted link whose TX
   notifications are enabled. It also goes out at once whenever such a link
   comes up.
2. A poke not answered by a valid `!TIME` within 30 s is a **miss**. A poke
   that falls due while no usable link exists is a miss at once, so an
   out-of-range or switched-off phone is counted too.
3. Any valid `!TIME` clears the count.
4. After 3 consecutive misses the ring is **blind**. Alert priority, highest
   first: a current low/high, then MISS, then STALE, then none. A current low
   or high still buzzes as such; otherwise a blind ring always buzzes MISS.
   **It is never silent while blind.**
5. **Alignment.** When a new reading is accepted, the next poke is aimed at
   `reading + 5 min + 2 min`, when the next reading should be in Nightscout.
   It is clamped to between 1 and 5 minutes from now, so it can bring pokes
   earlier but never make them rarer than the fixed cadence.

**MISS vs STALE.**

- **MISS:** the phone did not answer. The app was killed or force-quit, the
  phone is out of range, Bluetooth is off, or signing expired.
- **STALE:** the phone answered, but has no current reading. There's a CGM or
  Share outage, the sensor is warming up, Nightscout is down, or the token was
  revoked.

For the distinction to hold, **the app must send `!TIME` before its network
request**, so a slow or failed fetch never becomes a miss.

The MISS pattern is fixed (two 1000 ms pulses, full drive). `rc_validate()`
refuses it as a low/high/stale pattern, on exact match only.
