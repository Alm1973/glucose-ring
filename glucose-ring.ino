/*
 * glucose-ring.ino  --  haptic glucose alert ring, v2
 * Target: Seeed XIAO nRF52840 (plain BLE, NOT Sense)
 * FQBN:   Seeeduino:nrf52:xiaonRF52840   SoftDevice: S140 7.3.0
 *
 * *** SECONDARY ALERT ONLY. ***
 * A flat cell, a dropped link, or a hung sketch all fail silently from the
 * wearer's point of view. Keep the phone's own CGM alarms on.
 *
 * Protocol: see PROTOCOL.md.  Wiring and bring-up: see README.md.
 * Protocol 3: the ring drives the cycle -- it pokes the phone ("#REQ") every
 * ~5 min and alarms (MISS) when the phone stops answering.
 * Parsing, config, commands, and the clock/poke/alert rules live in the pure
 * modules (reading_parser / ring_config / command / liveness) and are covered
 * by tests/.
 */

#include <bluefruit.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

#include "reading_parser.h"
#include "ring_config.h"
#include "command.h"
#include "liveness.h"

using namespace Adafruit_LittleFS_Namespace;

/* ==================================================================
 * FACTORY DEFAULT THRESHOLDS
 * ==================================================================
 * Chosen by the wearer on 2026-09-12: low 60, high 160 mg/dL. Choosing
 * clinical numbers is not this firmware's job; these are theirs.
 *
 * The comparisons are strict: LOW fires when sgv < 60 (59 and under), HIGH
 * when sgv > 160 (161 and over).
 *
 * These seed the config file on FIRST boot only. After that, change them at
 * runtime with
 *     !SET THRESH <low> <high>
 * over BLE, which persists to flash -- no reflash needed. !RESET CONFIG
 * returns both sides to RC_UNSET, and an unset side never alerts.
 */
#define DEFAULT_LOW_MG_DL    60    // mg/dL, wearer's choice 2026-09-12
#define DEFAULT_HIGH_MG_DL   160   // mg/dL, wearer's choice 2026-09-12

/* A typo here would save an invalid config to flash on first boot, and
   nothing validates that write -- so refuse to build instead. */
static_assert(DEFAULT_LOW_MG_DL == RC_UNSET ||
              (DEFAULT_LOW_MG_DL >= RC_SGV_MIN && DEFAULT_LOW_MG_DL <= RC_SGV_MAX),
              "DEFAULT_LOW_MG_DL must be RC_UNSET or within RC_SGV_MIN..RC_SGV_MAX");
static_assert(DEFAULT_HIGH_MG_DL == RC_UNSET ||
              (DEFAULT_HIGH_MG_DL >= RC_SGV_MIN && DEFAULT_HIGH_MG_DL <= RC_SGV_MAX),
              "DEFAULT_HIGH_MG_DL must be RC_UNSET or within RC_SGV_MIN..RC_SGV_MAX");
static_assert(DEFAULT_LOW_MG_DL == RC_UNSET || DEFAULT_HIGH_MG_DL == RC_UNSET ||
              DEFAULT_LOW_MG_DL < DEFAULT_HIGH_MG_DL,
              "DEFAULT_LOW_MG_DL must be below DEFAULT_HIGH_MG_DL");

#if (DEFAULT_LOW_MG_DL == RC_UNSET) || (DEFAULT_HIGH_MG_DL == RC_UNSET)
  #warning "Threshold defaults are placeholders - that side never alerts until set (compile-time or !SET THRESH)."
#endif

/* ==================================================================
 * Compile-time options
 * ================================================================== */
#ifndef TEST_MODE
  #define TEST_MODE        0   // cycle every buzz pattern on boot
#endif
#ifndef DEBUG_SERIAL
  #define DEBUG_SERIAL     0   // USB serial tracing; costs power
#endif
#ifndef WATCHDOG_ENABLE
  #define WATCHDOG_ENABLE  1   // set 0 during bring-up if DFU misbehaves (see README)
#endif
#ifndef REQUIRE_BONDING
  #define REQUIRE_BONDING  1   // set 0 ONLY for bench work; see README security section
#endif

/* ==================================================================
 * Hardware
 * ==================================================================
 * Motor gate on D10 (P1.15). The pin only ever drives an AO3400 gate; the
 * ~85 mA motor current never touches the nRF52. External 10k gate pulldown
 * holds it off from power-on until setup() runs.
 *
 * D10 is on the same board edge as GND and 3V3, so the motor harness stays
 * on one side and no wire crosses the board.
 *
 * Tradeoff: variant.h:93 defines D10 as PIN_SPI_MOSI, so taking it means the
 * default SPI object can no longer be used. Nothing here needs SPI. If you
 * ever add an SPI peripheral, either move the motor or use SPI1 (P0.26).
 *
 * Verified: variant.cpp:19 maps D10 -> 47 -> P1.15 (port 1, pin 15), which
 * HardwarePWM::_set_psel writes straight into PSEL.OUT, so PWM intensity
 * works on this pin. P1.15 is neither an NFC pin (P0.09/P0.10) nor RESET.
 */
#define MOTOR_PIN            D10
#define BUZZ_MAX_ON_MS       2000    // hard cap on any single pulse

/*
 * Battery charge status. variant.h gives this pin no name, but
 * variants/Seeed_XIAO_nRF52840/variant.cpp:40 documents it:
 *     17, // D23 is P0.17 (~CHG)
 * It is the BQ25100's active-low charge-status output. (variant.cpp:39 is
 * D22 = P0.13 = HICHG, the charge-current select, which we never touch.)
 *
 * Careful: variant.h's PIN_WIRE1_SDA is index 17, which maps to P0.07 --
 * a different pin from P0.17. No conflict.
 */
#define PIN_CHG_STATUS       23      // D23 = P0.17, ~CHG, active LOW

/* Onboard divider: 1M (top) / 510k (bottom), gated by P0.14.
 * variant.h:56 -- "Output LOW to enable reading of the BAT voltage".
 * CALIBRATE VBAT_DIVIDER_COMP against a multimeter (see README). */
#define VBAT_MV_PER_LSB      (3000.0f / 4096.0f)   // AR_INTERNAL_3_0, 12-bit
#define VBAT_DIVIDER_COMP    (1510.0f / 510.0f)

#define VBAT_FLOOR_MV           3300
#define VBAT_WARN_MV            3450
#define VBAT_CHECK_INTERVAL_MS  60000UL
#define VBAT_FLOOR_CONFIRMS     5

/* ==================================================================
 * BLE / power
 * ==================================================================
 * Connection parameter tradeoff:
 *
 *   interval 200-400 ms, slave latency 4, supervision timeout 6000 ms
 *
 * Slave latency lets the ring skip up to 4 connection events when it has
 * nothing to say, so the radio effectively wakes every ~2 s while still
 * *appearing* to the phone as a 200-400 ms link -- the phone can reach us
 * within one interval whenever it actually has data. That is the whole
 * point of latency over simply using a 2 s interval: we get 2 s-ish idle
 * current with 400 ms-ish worst-case delivery once traffic starts.
 *
 * Bounded by Apple's Accessory Design Guidelines, which iOS enforces by
 * rejecting non-compliant requests outright:
 *   - interval >= 15 ms, and max >= min + 15 ms          -> 200/400 ok
 *   - interval_max * (latency + 1) <= 2 s                -> 400 * 5 = 2000 ok
 *   - supervision timeout <= 6 s                         -> 6000 ok
 *   - timeout > (1 + latency) * interval_max * 2 = 4 s   -> 6000 ok
 * Raising interval_max to 500 ms would break the 2 s rule, so 400 is the
 * ceiling here, not an arbitrary pick.
 *
 * CGM data arrives every ~5 min, so even a 2 s wake-up delay is irrelevant
 * to alert timeliness; the motor is what the wearer notices, and that is
 * gated by our own evaluate() cadence, not by the radio.
 */
#define DEVICE_NAME             "GlucoseRing"
#define CONN_INTERVAL_MIN_MS    200
#define CONN_INTERVAL_MAX_MS    400
#define CONN_SLAVE_LATENCY      4
#define CONN_TIMEOUT_MS         6000
#define TX_POWER_DBM            0
#define TICK_MS                 1000

#define RX_LINE_MAX             96      // longest !SET PATTERN line + slack
#define CONFIG_PATH             "/glucose-ring.cfg"
#define CONFIG_AUTOSAVE_MS      5000UL  // debounce flash writes after !SET

#define WATCHDOG_TIMEOUT_S      30

#if DEBUG_SERIAL
  #define DBG(...)   do { Serial.printf(__VA_ARGS__); } while (0)
#else
  #define DBG(...)   do { } while (0)
#endif

/* ================================================================== */

BLEUart bleuart;
BLEBas  blebas;          // standard Battery Service, 0x180F

enum BuzzId { BZ_LOW, BZ_HIGH, BZ_STALE, BZ_BATTERY, BZ_TICK, BZ_FAULT, BZ_GIVEUP, BZ_MISS };
/* Alert states (none/low/high/stale/miss) and their priority live in liveness.h. */

/* Battery and tick patterns are fixed; the three alert patterns are
   configurable and live in RingConfig. */
static const RcPulse PAT_BATTERY[] = { {600,250}, {300,250}, {150,0} };
static const RcPulse PAT_TICK[]    = { {80,0} };

/* Long-short-long. Deliberately unlike every other pattern: this one means
   "the firmware hung and the watchdog restarted me", which the wearer should
   be able to tell apart from any glucose condition. */
static const RcPulse PAT_FAULT[]   = { {150,80}, {60,80}, {150,0} };

/* Three ticks then one long: "I am giving up on the stale alert." Ends on a
   long pulse so it reads as terminal rather than as another repeat. */
static const RcPulse PAT_GIVEUP[]  = { {60,60}, {60,60}, {60,60}, {400,0} };

/* MISS ("the phone stopped answering") plays RC_PAT_MISS from ring_config:
   two long pulses at full drive. It is fixed, and refused as a low/high/stale
   pattern, so it can never feel like a glucose alert. */

static RingConfig g_cfg;
static bool       g_cfgDirty     = false;
static uint64_t   g_cfgDirtyMs   = 0;

static RpReading  g_last;
static bool       g_haveReading  = false;
static uint64_t   g_lastDataMs   = 0;      // when g_last ARRIVED -- for age_s only

/* Protocol 3. Wall time comes only from the phone's !TIME, never from when a
   reading happens to arrive; the poke state drives "#REQ" and the miss count. */
static LvClock    g_clock;
static LvPoke     g_poke;
static bool       g_linkWasReady = false;

static LvAlert    g_alert        = LV_ALERT_NONE;
static uint64_t   g_lastBuzzMs   = 0;
static uint8_t    g_staleRepeats = 0;      // consecutive stale buzzes
static bool       g_staleGaveUp  = false;  // capped out; silent until fresh data
static uint64_t   g_lastVbatMs   = 0;
static uint8_t    g_vbatLowCount = 0;
static bool       g_vbatWarned   = false;
static uint16_t   g_lastVbatMv   = 0;
static uint8_t    g_lastVbatPct  = 0;
static int        g_lastChg      = -1;
static uint64_t   g_bootMs       = 0;
static bool       g_bootFromWdt  = false;

static char    g_rxLine[RX_LINE_MAX];
static uint16_t g_rxLen      = 0;
static bool    g_rxOverflow = false;

/* ---- clock -------------------------------------------------------- */

static uint64_t millis64(void)
{
  static uint32_t last  = 0;
  static uint32_t wraps = 0;
  uint32_t now = millis();
  if (now < last) wraps++;      // millis() wraps every ~49.7 days
  last = now;
  return ((uint64_t)wraps << 32) | (uint64_t)now;
}

/* ---- watchdog -----------------------------------------------------
 * Register-level: this core ships no WDT wrapper (checked cores/nRF5).
 * Field names verified in nrfx/mdk/nrf52840_bitfields.h.
 *
 * CONFIG.SLEEP must be Run: we spend nearly all our time in WFI, and with
 * SLEEP=Pause the counter would effectively never advance, making the
 * watchdog decorative.
 *
 * Once started the nRF52 WDT cannot be stopped or reconfigured. HALT=Pause
 * keeps a debugger session from tripping it.
 */
#if WATCHDOG_ENABLE
static void wdtStart(uint32_t timeout_s)
{
  NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos) |
                    (WDT_CONFIG_SLEEP_Run  << WDT_CONFIG_SLEEP_Pos);
  NRF_WDT->CRV    = (32768UL * timeout_s) - 1UL;   // LFCLK is 32.768 kHz
  NRF_WDT->RREN   = (WDT_RREN_RR0_Enabled << WDT_RREN_RR0_Pos);
  NRF_WDT->TASKS_START = 1;
}
static inline void wdtFeed(void) { NRF_WDT->RR[0] = WDT_RR_RR_Reload; }
#else
static void wdtStart(uint32_t) {}
static inline void wdtFeed(void) {}
#endif

/* ---- motor --------------------------------------------------------
 * Intensity is PWM duty on the MOSFET gate. The PWM peripheral is taken,
 * used, and fully released around each buzz -- leaving it enabled would
 * burn current continuously and wreck the runtime budget.
 *
 * A vibration motor will not start from a low duty (stiction), so each
 * pulse begins with a short full-drive kick before dropping to the
 * configured level.
 */
#define BUZZ_PWM_TOKEN   0x47524E47UL   // 'GRNG'
#define BUZZ_KICK_MS     25

static HardwarePWM *s_pwm = NULL;

static void motorHardOff(void)
{
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
}

static void motorPwmRelease(void)
{
  if (s_pwm) {
    s_pwm->writePin(MOTOR_PIN, 0);
    s_pwm->removePin(MOTOR_PIN);   // must precede releaseOwnership()
    s_pwm->stop();                 // ENABLE = 0
    s_pwm->releaseOwnership(BUZZ_PWM_TOKEN);
    s_pwm = NULL;
  }
  motorHardOff();
}

static bool motorPwmAcquire(void)
{
  if (s_pwm) return true;
  for (int i = 0; i < HWPWM_MODULE_NUM; i++) {
    if (HwPWMx[i]->takeOwnership(BUZZ_PWM_TOKEN)) {
      s_pwm = HwPWMx[i];
      s_pwm->setResolution(8);     // duty 0..255
      s_pwm->addPin(MOTOR_PIN);
      return true;
    }
  }
  return false;                    // fall back to full-on digital drive
}

static void runPattern(const RcPulse *p, uint8_t n, uint8_t intensity)
{
  if (intensity > 100) intensity = 100;
  uint32_t duty = (255UL * intensity) / 100UL;
  if (duty == 0) duty = 1;

  bool pwm = (intensity < 100) && motorPwmAcquire();

  for (uint8_t i = 0; i < n; i++) {
    uint16_t on = p[i].on_ms > BUZZ_MAX_ON_MS ? BUZZ_MAX_ON_MS : p[i].on_ms;

    if (pwm) {
      s_pwm->writePin(MOTOR_PIN, 255);            // kick through stiction
      delay(on < BUZZ_KICK_MS ? on : BUZZ_KICK_MS);
      if (on > BUZZ_KICK_MS) {
        s_pwm->writePin(MOTOR_PIN, (uint16_t)duty);
        delay(on - BUZZ_KICK_MS);
      }
      s_pwm->writePin(MOTOR_PIN, 0);
    } else {
      digitalWrite(MOTOR_PIN, HIGH);
      delay(on);
      digitalWrite(MOTOR_PIN, LOW);
    }

    if (p[i].off_ms) delay(p[i].off_ms);
    wdtFeed();                                     // long patterns must not trip it
  }

  motorPwmRelease();
}

static void buzz(BuzzId id)
{
  switch (id) {
    case BZ_LOW:     runPattern(g_cfg.pat_low.pulses,   g_cfg.pat_low.count,   g_cfg.intensity); break;
    case BZ_HIGH:    runPattern(g_cfg.pat_high.pulses,  g_cfg.pat_high.count,  g_cfg.intensity); break;
    case BZ_STALE:   runPattern(g_cfg.pat_stale.pulses, g_cfg.pat_stale.count, g_cfg.intensity); break;
    case BZ_BATTERY: runPattern(PAT_BATTERY, 3, 100); break;   // always full drive
    case BZ_TICK:    runPattern(PAT_TICK,    1, g_cfg.intensity); break;
    case BZ_FAULT:   runPattern(PAT_FAULT,   3, 100); break;   // full drive: must not be missed
    case BZ_GIVEUP:  runPattern(PAT_GIVEUP,  4, g_cfg.intensity); break;
    case BZ_MISS:    runPattern(RC_PAT_MISS.pulses, RC_PAT_MISS.count, 100); break;   // full drive
    default:         motorHardOff(); break;
  }
}

static bool buzzByName(const char *name)
{
  if (!strcmp(name, "low"))     { buzz(BZ_LOW);     return true; }
  if (!strcmp(name, "high"))    { buzz(BZ_HIGH);    return true; }
  if (!strcmp(name, "stale"))   { buzz(BZ_STALE);   return true; }
  if (!strcmp(name, "battery")) { buzz(BZ_BATTERY); return true; }
  if (!strcmp(name, "tick"))    { buzz(BZ_TICK);    return true; }
  if (!strcmp(name, "fault"))   { buzz(BZ_FAULT);   return true; }
  if (!strcmp(name, "giveup"))  { buzz(BZ_GIVEUP);  return true; }
  if (!strcmp(name, "miss"))    { buzz(BZ_MISS);    return true; }
  return false;
}

/* ---- charger detection --------------------------------------------
 * Two independent signals, either of which means "do not sample VBAT":
 *
 *  1. ~CHG (P0.17) low  -- BQ25100 actively charging.
 *  2. USB VBUS present  -- the rail may be held up by USB even when the
 *     charger has terminated, so a reading then is not the cell's voltage.
 *
 * ~CHG is open-drain on the BQ25100, so it needs a pull-up to read a
 * reliable high. We enable the internal pull-up only for the microseconds
 * of the read, then release the pin to high-Z so it costs nothing.
 *
 * NRF_POWER is a SoftDevice-restricted peripheral once the stack is
 * enabled, so VBUS is read via sd_power_usbregstatus_get()
 * (nrf_soc.h:586, S140 7.3.0) rather than by touching the register.
 */
static int chargerActive(void)
{
  pinMode(PIN_CHG_STATUS, INPUT_PULLUP);
  delayMicroseconds(100);                 // let the pull-up settle
  bool charging = (digitalRead(PIN_CHG_STATUS) == LOW);
  pinMode(PIN_CHG_STATUS, INPUT);         // release: no standing pull-up current

  uint32_t usb = 0;
  uint8_t sd_on = 0;
  sd_softdevice_is_enabled(&sd_on);
  if (sd_on) {
    if (sd_power_usbregstatus_get(&usb) != NRF_SUCCESS) usb = 0;
  } else {
    usb = NRF_POWER->USBREGSTATUS;
  }
  bool vbus = (usb & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;

  return (charging || vbus) ? 1 : 0;
}

/* ---- battery ------------------------------------------------------ */

/* Rough Li-ion open-circuit curve. Approximate by nature -- it is a
 * display convenience, not the basis of the cutoff, which uses raw mV. */
static uint8_t vbatPercent(uint16_t mv)
{
  static const uint16_t curve_mv[]  = { 3300, 3600, 3700, 3750, 3850, 3950, 4050, 4200 };
  static const uint8_t  curve_pct[] = {    0,   10,   25,   40,   60,   75,   90,  100 };
  const int n = (int)(sizeof(curve_mv) / sizeof(curve_mv[0]));

  if (mv <= curve_mv[0])     return 0;
  if (mv >= curve_mv[n - 1]) return 100;
  for (int i = 1; i < n; i++) {
    if (mv < curve_mv[i]) {
      uint16_t span = (uint16_t)(curve_mv[i] - curve_mv[i - 1]);
      uint8_t  drop = (uint8_t)(curve_pct[i] - curve_pct[i - 1]);
      return (uint8_t)(curve_pct[i - 1] + ((uint32_t)(mv - curve_mv[i - 1]) * drop) / span);
    }
  }
  return 100;
}

static uint16_t readVbatMv(void)
{
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW);        // LOW enables the divider (variant.h:56)
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  delay(5);

  (void)analogRead(PIN_VBAT);            // discard first conversion
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) acc += analogRead(PIN_VBAT);

  /* Release to high-Z, don't drive HIGH: P0.14 is the low leg of the
     divider, so parking it at 3V3 still leaks ~0.6 uA from a 4.2 V cell,
     and driving it up momentarily lifts the P0.31 node above VDD.
     Enable window is ~6 ms once a minute -- about 0.01% duty. */
  pinMode(VBAT_ENABLE, INPUT);

  float raw = acc / 16.0f;
  return (uint16_t)(raw * VBAT_MV_PER_LSB * VBAT_DIVIDER_COMP);
}

static void powerOffForever(void)
{
  motorHardOff();
  Bluefruit.Advertising.stop();

  uint8_t sd_enabled = 0;
  sd_softdevice_is_enabled(&sd_enabled);
  if (sd_enabled) sd_power_system_off();
  else            NRF_POWER->SYSTEMOFF = 1;

  while (1) __WFE();   // only a reset or a charge cycle comes back
}

static void checkBattery(uint64_t now)
{
  if (now - g_lastVbatMs < VBAT_CHECK_INTERVAL_MS) return;
  g_lastVbatMs = now;

  /*
   * Documented burnout risk on P0.31: do not sample while the charger is
   * driving the BAT node. A charging cell also reads high, so the sample
   * would be meaningless even if it were safe.
   */
  g_lastChg = chargerActive();
  if (g_lastChg == 1) {
    g_vbatLowCount = 0;          // never shut down while on the charger
    DBG("vbat: skipped, charger active\n");
    return;
  }

  uint16_t mv = readVbatMv();
  g_lastVbatMv  = mv;
  g_lastVbatPct = vbatPercent(mv);
  blebas.write(g_lastVbatPct);
  DBG("vbat %u mV (%u%%)\n", mv, g_lastVbatPct);

  if (mv < VBAT_FLOOR_MV) {
    /* The motor sags the rail hard; demand repeated confirmation rather
       than shutting down on one dip. */
    if (++g_vbatLowCount >= VBAT_FLOOR_CONFIRMS) {
      DBG("vbat floor -> system off\n");
      buzz(BZ_BATTERY);
      powerOffForever();
    }
  } else {
    g_vbatLowCount = 0;
    if (!g_vbatWarned && mv < VBAT_WARN_MV) {
      g_vbatWarned = true;
      buzz(BZ_BATTERY);
    }
  }
}

/* ---- config persistence -------------------------------------------
 * Adafruit_LittleFS.h:48 warns only one file may be open at a time, and
 * the Bluefruit stack stores bond keys in this same filesystem
 * (utility/bonding.h:41, /adafruit/bond_prph). So: open, do the I/O,
 * close immediately. Never hold a handle across anything that could let
 * the stack write a bond.
 */
static bool configSave(void)
{
  char buf[RC_SERIAL_MAX];
  int n = rc_serialize(&g_cfg, buf, sizeof(buf));
  if (n < 0) { DBG("cfg: serialize failed\n"); return false; }

  InternalFS.remove(CONFIG_PATH);        // LittleFS append-opens otherwise
  File f = InternalFS.open(CONFIG_PATH, FILE_O_WRITE);
  if (!f) { DBG("cfg: open for write failed\n"); return false; }
  size_t wrote = f.write((const uint8_t *)buf, (size_t)n);
  f.close();

  bool ok = (wrote == (size_t)n);
  DBG("cfg: saved %u bytes (%s)\n", (unsigned)wrote, ok ? "ok" : "SHORT");
  if (ok) { g_cfgDirty = false; }
  return ok;
}

static void configLoad(void)
{
  rc_defaults(&g_cfg);
  g_cfg.low_mg_dl  = DEFAULT_LOW_MG_DL;
  g_cfg.high_mg_dl = DEFAULT_HIGH_MG_DL;

  if (!InternalFS.exists(CONFIG_PATH)) {
    DBG("cfg: none, writing defaults\n");
    configSave();
    return;
  }

  File f = InternalFS.open(CONFIG_PATH, FILE_O_READ);
  if (!f) { DBG("cfg: open failed, using defaults\n"); return; }

  char buf[RC_SERIAL_MAX];
  int n = f.read(buf, (uint16_t)(sizeof(buf) - 1));
  f.close();
  if (n <= 0) { DBG("cfg: empty, using defaults\n"); return; }
  buf[n] = '\0';

  RingConfig loaded;
  const char *why = NULL;
  if (rc_deserialize(buf, &loaded, &why)) {
    g_cfg = loaded;
    DBG("cfg: loaded\n");
  } else {
    /* A corrupt or truncated file must not install a half-valid config.
       Fall back to defaults and rewrite so the next boot is clean. */
    DBG("cfg: rejected (%s), rewriting defaults\n", why ? why : "?");
    configSave();
  }
}

static void configMarkDirty(void)
{
  g_cfgDirty   = true;
  g_cfgDirtyMs = millis64();
}

/* Debounced write-back: a phone that spams !SET must not chew through
   flash erase cycles. */
static void configServiceAutosave(uint64_t now)
{
  if (g_cfgDirty && (now - g_cfgDirtyMs) >= CONFIG_AUTOSAVE_MS) configSave();
}

/* ---- security ------------------------------------------------------
 * This core's BLEUart::begin() hardcodes SECMODE_OPEN on both
 * characteristics (services/BLEUart.cpp, with the library's own
 * "TODO enable encryption when bonding is enabled"), and setPermission()
 * only fills _attr_meta, which begin() has already consumed. So GATT-level
 * encryption on NUS is not reachable without forking library internals.
 *
 * Instead the gate is enforced here: an unpaired peer may connect and
 * write bytes, but every byte is discarded, so it cannot buzz the motor,
 * read state, or change config. See REPORT.md for what this does and does
 * not buy.
 */
static bool peerIsTrusted(uint16_t conn_hdl)
{
#if REQUIRE_BONDING
  BLEConnection *c = Bluefruit.Connection(conn_hdl);
  if (!c) return false;
  return c->secured();
#else
  (void)conn_hdl;
  return true;
#endif
}

/* ---- command host bindings ---------------------------------------- */

static LvData dataState(uint64_t now);

static CmdHost g_host;

static void hostInit(void)
{
  memset(&g_host, 0, sizeof(g_host));
  g_host.ctx = NULL;
  g_host.batt_mv       = [](void *) -> uint16_t { return g_lastVbatMv; };
  g_host.batt_pct      = [](void *) -> uint8_t  { return g_lastVbatPct; };
  g_host.charging      = [](void *) -> int      { return g_lastChg; };
  g_host.have_reading  = [](void *) -> int      { return g_haveReading ? 1 : 0; };
  g_host.sgv           = [](void *) -> int32_t  { return g_last.sgv; };
  g_host.direction     = [](void *) -> const char * { return g_last.direction; };
  g_host.reading_age_s = [](void *) -> uint32_t {
      return g_haveReading ? (uint32_t)((millis64() - g_lastDataMs) / 1000ULL) : 0U; };
  g_host.is_stale      = [](void *) -> int      { return dataState(millis64()) == LV_DATA_NOT_CURRENT ? 1 : 0; };
  g_host.alert_name    = [](void *) -> const char * { return lv_alert_name(g_alert); };
  g_host.uptime_s      = [](void *) -> uint32_t { return (uint32_t)((millis64() - g_bootMs) / 1000ULL); };
  g_host.buzz          = [](void *, const char *n) -> int { return buzzByName(n) ? 1 : 0; };
  g_host.save          = [](void *) -> int      { return configSave() ? 1 : 0; };
  g_host.mark_dirty    = [](void *)              { configMarkDirty(); };
  g_host.factory_reset = [](void *) -> int       { return configSave() ? 1 : 0; };

  /* protocol 3 */
  g_host.set_time      = [](void *, uint64_t ms) -> int {
      lv_clock_sync(&g_clock, ms, millis64());
      lv_poke_answered(&g_poke);        // proof of life: clears the miss count
      return 1; };
  g_host.link_misses   = [](void *) -> int      { return (int)g_poke.misses; };
  g_host.clock_synced  = [](void *) -> int      { return g_clock.synced ? 1 : 0; };
  g_host.next_poke_s   = [](void *) -> uint32_t {
      uint64_t now = millis64();
      return g_poke.next_poke_ms > now ? (uint32_t)((g_poke.next_poke_ms - now) / 1000ULL) : 0U; };
}

/* ---- line handling ------------------------------------------------ */

static void sendLine(const char *s)
{
  bleuart.write((const uint8_t *)s, strlen(s));
  bleuart.write((const uint8_t *)"\n", 1);
}

static void handleReading(const char *line, uint64_t now)
{
  RpLimits lim = RP_DEFAULT_LIMITS;
  RpReading r;
  RpResult res = rp_parse(line, &lim, &r);
  if (res != RP_OK) {
    DBG("drop: %s\n", rp_result_name(res));
    return;
  }

  /* Replays and out-of-order pushes must not clear staleness: a re-sent
     old value still means we have no current data. */
  if (g_haveReading && r.epoch_ms <= g_last.epoch_ms) {
    DBG("drop: not newer\n");
    return;
  }

  /* Judge the reading by its OWN timestamp against the phone's !TIME, never
     by when it arrived. Protocol 2 learned the clock from the readings
     themselves, so after a reboot a 40-minute-old first reading looked
     fresh. Now: no !TIME since boot, no reading. Too old, or dated in the
     future (which would lock out every later reading): no reading. */
  LvVerdict v = lv_judge(&g_clock, now, r.epoch_ms, (uint64_t)g_cfg.stale_minutes * 60000ULL);
  if (v != LV_FRESH) {
    DBG("drop: %s\n", lv_verdict_name(v));
    return;
  }

  g_last        = r;
  g_haveReading = true;
  g_lastDataMs  = now;
  lv_poke_align(&g_poke, &g_clock, now, r.epoch_ms);   // aim the next poke at the next reading
  DBG("sgv %ld %s\n", (long)r.sgv, r.direction);
}

static void handleLine(char *line, uint64_t now)
{
  if (cmd_is_command(line)) {
    char resp[CMD_RESP_MAX];
    cmd_execute(line, &g_cfg, &g_host, resp, sizeof(resp));
    sendLine(resp);
  } else {
    handleReading(line, now);
  }
}

/*
 * Announce ourselves once per connection, but only after the link is
 * trusted -- an unpaired peer gets nothing, not even a version string.
 *
 * This is how a watchdog reset becomes visible: the whole point of the
 * WDT is that a hang does not leave the ring silently dead, so the phone
 * must be able to see that a reset happened rather than just noticing the
 * uptime looks short.
 */
static bool g_bootAnnounced = false;

static void announceBoot(void)
{
  if (g_bootAnnounced) return;
  if (!Bluefruit.connected()) return;
  if (!peerIsTrusted(Bluefruit.connHandle())) return;

  char msg[96];
  snprintf(msg, sizeof(msg), "#BOOT ver=%s proto=%d wdt_reset=%d up_s=%lu",
           FW_VERSION, PROTO_VERSION, g_bootFromWdt ? 1 : 0,
           (unsigned long)((millis64() - g_bootMs) / 1000ULL));
  sendLine(msg);
  g_bootAnnounced = true;
}

static void pumpUart(uint64_t now)
{
  while (bleuart.available()) {
    int ci = bleuart.read();
    if (ci < 0) break;

    /* Drain and discard from an unpaired peer rather than leaving bytes
       in the FIFO to be replayed after a later legitimate pairing. */
    if (!peerIsTrusted(Bluefruit.connHandle())) { g_rxLen = 0; g_rxOverflow = false; continue; }

    char c = (char)ci;
    if (c == '\n') {
      if (!g_rxOverflow) { g_rxLine[g_rxLen] = '\0'; handleLine(g_rxLine, now); }
      g_rxLen = 0;
      g_rxOverflow = false;
      continue;
    }
    if (c == '\r') continue;

    if (g_rxLen >= RX_LINE_MAX - 1) { g_rxOverflow = true; continue; }  // resync at next '\n'
    g_rxLine[g_rxLen++] = c;
  }
}

/* ---- the poke cycle (protocol 3) -----------------------------------
 * iOS will not let the phone poll on a timer in the background, but it wakes
 * a bluetooth-central app when a connected peripheral notifies. So the ring
 * drives the cycle: every ~5 min it sends "#REQ"; the phone answers
 * "!TIME <epoch_ms>" and then the reading. Rules: liveness.h.
 *
 * This is a check in the existing 1 s loop, not a new wake source: the ring
 * already wakes every TICK_MS and already holds the connection.
 */
static bool linkReady(void)
{
  if (!Bluefruit.connected()) return false;
  if (!peerIsTrusted(Bluefruit.connHandle())) return false;
  return bleuart.notifyEnabled();          // the phone has subscribed to TX
}

static void serviceWake(uint64_t now)
{
  bool ready = linkReady();
  if (ready && !g_linkWasReady) lv_poke_link_up(&g_poke, now);   // fresh link: ask now
  g_linkWasReady = ready;

  uint8_t before = g_poke.misses;
  if (lv_poke_tick(&g_poke, now, ready) == LV_SEND_POKE) sendLine("#REQ");
  if (g_poke.misses != before) DBG("poke: miss %u\n", (unsigned)g_poke.misses);
}

/* ---- alert state machine ------------------------------------------ */

static LvData dataState(uint64_t now)
{
  return lv_data_state(&g_clock, now, g_haveReading, g_last.epoch_ms,
                       (uint64_t)g_cfg.stale_minutes * 60000ULL, g_bootMs);
}

static LvAlert evaluate(uint64_t now)
{
  /* Stale outranks the thresholds (never act on old data), and a phone that
     has stopped answering outranks "none": see lv_select(). */
  return lv_select(dataState(now), g_last.sgv, g_cfg.low_mg_dl, g_cfg.high_mg_dl,
                   lv_blind(&g_poke));
}

static void serviceAlerts(uint64_t now)
{
  LvAlert want  = evaluate(now);
  bool    noData = (want == LV_ALERT_STALE || want == LV_ALERT_MISS);

  /* The repeat budget covers the two "no current data" alarms. It rearms on
     any other state, and on a switch between them: STALE -> MISS says
     something new (the phone itself has gone quiet). */
  if (!noData || want != g_alert) {
    g_staleRepeats = 0;
    g_staleGaveUp  = false;
  }

  if (want == LV_ALERT_NONE) { g_alert = LV_ALERT_NONE; return; }

  /* Capped out: hold the state so !GET STATUS still reports it, but stop
     buzzing. A ring buzzing in a drawer for hours trains the wearer to ignore
     it, and an ignored alert is worse than a quiet one. */
  if (noData && g_staleGaveUp) { g_alert = want; return; }

  bool changed = (want != g_alert);
  bool due     = (now - g_lastBuzzMs) >= ((uint64_t)g_cfg.repeat_minutes * 60000ULL);
  if (!changed && !due) return;

  g_alert      = want;
  g_lastBuzzMs = now;

  if (changed) {
    char msg[64];
    if (g_haveReading) snprintf(msg, sizeof(msg), "#ALERT %s sgv=%ld", lv_alert_name(want), (long)g_last.sgv);
    else               snprintf(msg, sizeof(msg), "#ALERT %s", lv_alert_name(want));
    sendLine(msg);
  }

  switch (want) {
    case LV_ALERT_LOW:   buzz(BZ_LOW);   break;
    case LV_ALERT_HIGH:  buzz(BZ_HIGH);  break;
    case LV_ALERT_STALE: buzz(BZ_STALE); break;
    case LV_ALERT_MISS:  buzz(BZ_MISS);  break;
    default: break;
  }

  if (noData) {
    if (g_staleRepeats < 255) g_staleRepeats++;
    /* stale_max_repeats == 0 means "never stop". */
    if (g_cfg.stale_max_repeats != 0 && g_staleRepeats >= g_cfg.stale_max_repeats) {
      g_staleGaveUp = true;
      sendLine(want == LV_ALERT_MISS ? "#ALERT miss_giveup" : "#ALERT stale_giveup");
      buzz(BZ_GIVEUP);      /* distinct: it gave up, it did not get fixed */
      DBG("%s: gave up after %u repeats\n", lv_alert_name(want), (unsigned)g_staleRepeats);
    }
  }
}

/* ---- BLE callbacks ------------------------------------------------- */

static void onConnect(uint16_t handle)
{
  BLEConnection *c = Bluefruit.Connection(handle);
  if (c) {
    c->requestConnectionParameter(
        (uint16_t)(CONN_INTERVAL_MAX_MS / 1.25f),   // units of 1.25 ms
        CONN_SLAVE_LATENCY,
        (uint16_t)(CONN_TIMEOUT_MS / 10));          // units of 10 ms
#if REQUIRE_BONDING
    if (!c->secured()) c->requestPairing();
#endif
  }
  DBG("connected\n");
}

static void onDisconnect(uint16_t handle, uint8_t reason)
{
  (void)handle; (void)reason;
  motorHardOff();          // never leave the motor latched on
  g_rxLen = 0;
  g_rxOverflow = false;
  g_bootAnnounced = false;
  DBG("disconnected (0x%02X)\n", reason);
  /* Re-advertising is automatic: restartOnDisconnect(true) in setup(). */
}

/* ==================================================================== */

void setup()
{
  /* Motor off before anything else can take time. */
  motorHardOff();

  /* Read the reset reason before the SoftDevice claims NRF_POWER. */
  uint32_t reas = NRF_POWER->RESETREAS;
  NRF_POWER->RESETREAS = reas;                 // write 1s to clear
  g_bootFromWdt = (reas & POWER_RESETREAS_DOG_Msk) != 0;

  /* XIAO RGB LEDs are common-anode: HIGH = off. Leaving one lit would
     swamp the entire power budget. */
  pinMode(LED_RED, OUTPUT);   digitalWrite(LED_RED, HIGH);
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_BLUE, OUTPUT);  digitalWrite(LED_BLUE, HIGH);

#if DEBUG_SERIAL
  Serial.begin(115200);
  delay(2000);            // never block on `while(!Serial)`: headless it never returns
#endif

  InternalFS.begin();
  configLoad();
  hostInit();

  Bluefruit.autoConnLed(false);
  Bluefruit.configPrphBandwidth(BANDWIDTH_LOW);
  Bluefruit.begin(1, 0);
  Bluefruit.setTxPower(TX_POWER_DBM);
  Bluefruit.setName(DEVICE_NAME);
  Bluefruit.Periph.setConnIntervalMS(CONN_INTERVAL_MIN_MS, CONN_INTERVAL_MAX_MS);
  Bluefruit.Periph.setConnSlaveLatency(CONN_SLAVE_LATENCY);
  Bluefruit.Periph.setConnSupervisionTimeoutMS(CONN_TIMEOUT_MS);
  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);

  blebas.begin();
  blebas.write(100);        // replaced by the first real reading
  bleuart.begin();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(244, 1600);   // 152.5 ms fast, 1 s slow
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);                 // 0 = advertise forever

  g_bootMs     = millis64();
  g_lastDataMs = g_bootMs;
  g_lastVbatMs = g_bootMs;
  lv_clock_init(&g_clock);                        // no wall time until the phone's !TIME
  lv_poke_init(&g_poke, g_bootMs);                // first "#REQ" on link-up, else in 5 min

#if TEST_MODE
  DBG("TEST_MODE: cycling patterns\n");
  delay(500);
  buzz(BZ_TICK);    delay(800);
  buzz(BZ_LOW);     delay(800);
  buzz(BZ_HIGH);    delay(800);
  buzz(BZ_STALE);   delay(800);
  buzz(BZ_MISS);    delay(800);
  buzz(BZ_BATTERY);
  DBG("TEST_MODE: chg=%d vbat=%u mV\n", chargerActive(), readVbatMv());
#endif

  /*
   * If the watchdog restarted us, say so with the motor as well as over BLE.
   * A fault that silently restarted the ring is exactly the thing the wearer
   * should know happened -- BLE alone only reaches them if a phone is
   * connected and looking.
   */
  if (g_bootFromWdt) {
    DBG("boot: watchdog reset\n");
    buzz(BZ_FAULT);
  }

  /* Started last: everything above must complete inside one timeout. */
  wdtStart(WATCHDOG_TIMEOUT_S);
}

void loop()
{
  uint64_t now = millis64();

  wdtFeed();
  announceBoot();
  pumpUart(now);
  serviceWake(now);
  serviceAlerts(now);
  checkBattery(now);
  configServiceAutosave(now);

  /* FreeRTOS delay -- the idle task drops the core into WFI while the
     SoftDevice keeps servicing the radio underneath. */
  delay(TICK_MS);
}
