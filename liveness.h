/*
 * liveness.h -- is the phone answering, and is the reading current?
 *
 * PURE: no Arduino, no radio, no globals. The sketch owns the clock and the
 * radio; this module owns the rules, so they are host-testable
 * (tests/test_all.cpp) -- the same split as reading_parser.
 *
 * Protocol 3 inverts the cycle for iOS: the ring pokes ("#REQ"), the phone
 * answers ("!TIME <epoch_ms>", then the reading). Two questions follow, kept
 * apart on purpose:
 *
 *   Is the phone answering?  poke / miss accounting  -> MISS alarm
 *   Is the reading current?  trusted clock            -> STALE
 *
 * A silent ring must never be ambiguous between "all fine" and "no data":
 * lv_select() never returns NONE once the phone has missed
 * LV_MISS_ALERT_COUNT pokes in a row.
 */
#ifndef LIVENESS_H_
#define LIVENESS_H_

#include <stdint.h>

/* ---- named constants. Compile-time on purpose: not changeable over BLE. ---- */
#define LV_POKE_INTERVAL_MS     300000ULL  /* 5 min: Dexcom's cadence                        */
#define LV_ANSWER_TIMEOUT_MS     30000ULL  /* the phone must answer a poke within this        */
#define LV_MISS_ALERT_COUNT          3     /* consecutive misses -> MISS (~15 min blind)       */
#define LV_ALIGN_LAG_MS         120000ULL  /* ASSUMED CGM->Nightscout lag; tune on real data   */
#define LV_MIN_POKE_GAP_MS       60000ULL  /* alignment never pokes sooner than this           */
#define LV_FUTURE_TOLERANCE_MS  120000ULL  /* clock skew allowed before "dated in the future"  */

/* ---- trusted clock: wall time as told by the phone's !TIME ---- */
typedef struct {
  bool     synced;
  uint64_t epoch_at_sync;   /* phone's epoch ms, from !TIME           */
  uint64_t local_at_sync;   /* ring's millis64() when !TIME arrived   */
} LvClock;

void lv_clock_init(LvClock *c);
void lv_clock_sync(LvClock *c, uint64_t phone_epoch_ms, uint64_t local_ms);
/* Current wall time; false if the phone has not sent !TIME since boot. */
bool lv_clock_now(const LvClock *c, uint64_t local_ms, uint64_t *epoch_ms);

typedef enum { LV_FRESH = 0, LV_STALE, LV_NO_CLOCK, LV_FUTURE } LvVerdict;

/*
 * Judge a reading by ITS OWN timestamp against trusted time -- never by when
 * it arrived. Age == stale_ms is still fresh; one ms more is stale.
 */
LvVerdict   lv_judge(const LvClock *c, uint64_t local_ms, uint64_t reading_epoch_ms,
                     uint64_t stale_ms);
const char *lv_verdict_name(LvVerdict v);

/* ---- pokes and misses ---- */
typedef struct {
  uint64_t next_poke_ms;   /* local ms of the next "#REQ"           */
  uint64_t deadline_ms;    /* answer deadline for the pending poke  */
  bool     awaiting;       /* a poke is out and unanswered          */
  uint8_t  misses;         /* consecutive, saturating at 255        */
} LvPoke;

typedef enum { LV_NOTHING = 0, LV_SEND_POKE } LvAction;

void     lv_poke_init(LvPoke *p, uint64_t now_ms);
/*
 * Call every loop. Returns LV_SEND_POKE when "#REQ" is due and the link can
 * carry it. A poke that falls due while the link is down counts as a miss at
 * once -- no answer is possible -- so an out-of-range phone is caught too.
 */
LvAction lv_poke_tick(LvPoke *p, uint64_t now_ms, bool link_ready);
/* A valid !TIME arrived: the phone is alive. Clears the miss count. */
void     lv_poke_answered(LvPoke *p);
/* The link just became usable: poke now rather than wait out the cadence. */
void     lv_poke_link_up(LvPoke *p, uint64_t now_ms);
/*
 * A new fresh reading was accepted: aim the next poke at when the NEXT
 * reading should have reached Nightscout (reading + interval + lag), clamped
 * to [now + LV_MIN_POKE_GAP_MS, now + LV_POKE_INTERVAL_MS] so alignment can
 * never make the ring poke less often than the fixed cadence.
 */
void     lv_poke_align(LvPoke *p, const LvClock *c, uint64_t now_ms, uint64_t reading_epoch_ms);
bool     lv_blind(const LvPoke *p);

/* ---- alert selection ---- */
typedef enum { LV_DATA_GRACE = 0, LV_DATA_CURRENT, LV_DATA_NOT_CURRENT } LvData;
typedef enum { LV_ALERT_NONE = 0, LV_ALERT_LOW, LV_ALERT_HIGH, LV_ALERT_STALE, LV_ALERT_MISS } LvAlert;

/* GRACE = no reading yet, and still within one stale window of boot. */
LvData lv_data_state(const LvClock *c, uint64_t now_ms, bool have_reading,
                     uint64_t reading_epoch_ms, uint64_t stale_ms, uint64_t boot_ms);

/*
 * Priority, highest first:
 *   LOW / HIGH  -- only on a CURRENT reading (never act on stale data)
 *   MISS        -- the phone stopped answering (blind), whatever the data
 *   STALE       -- no current reading, but the phone is answering
 *   NONE
 * low/high take RC_UNSET for "this side never alerts".
 */
LvAlert     lv_select(LvData data, int32_t sgv, int32_t low, int32_t high, bool blind);
const char *lv_alert_name(LvAlert a);

#endif /* LIVENESS_H_ */
