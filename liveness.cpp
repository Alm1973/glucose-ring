#include "liveness.h"
#include "ring_config.h"   /* RC_UNSET */

/* ---- trusted clock ---- */

void lv_clock_init(LvClock *c)
{
  c->synced        = false;
  c->epoch_at_sync = 0;
  c->local_at_sync = 0;
}

void lv_clock_sync(LvClock *c, uint64_t phone_epoch_ms, uint64_t local_ms)
{
  c->synced        = true;
  c->epoch_at_sync = phone_epoch_ms;
  c->local_at_sync = local_ms;
}

bool lv_clock_now(const LvClock *c, uint64_t local_ms, uint64_t *epoch_ms)
{
  if (!c || !c->synced || !epoch_ms) return false;
  /* Local time is monotonic; a sample from before the sync clamps to it
     rather than running the wall clock backwards. */
  uint64_t elapsed = local_ms > c->local_at_sync ? local_ms - c->local_at_sync : 0;
  *epoch_ms = c->epoch_at_sync + elapsed;
  return true;
}

LvVerdict lv_judge(const LvClock *c, uint64_t local_ms, uint64_t reading_epoch_ms,
                   uint64_t stale_ms)
{
  uint64_t now;
  if (!lv_clock_now(c, local_ms, &now))                   return LV_NO_CLOCK;
  if (reading_epoch_ms > now + LV_FUTURE_TOLERANCE_MS)    return LV_FUTURE;
  if (now > reading_epoch_ms && now - reading_epoch_ms > stale_ms) return LV_STALE;
  return LV_FRESH;
}

const char *lv_verdict_name(LvVerdict v)
{
  switch (v) {
    case LV_FRESH:    return "FRESH";
    case LV_STALE:    return "STALE";
    case LV_NO_CLOCK: return "NO_CLOCK";
    case LV_FUTURE:   return "FUTURE";
    default:          return "?";
  }
}

/* ---- pokes and misses ---- */

static void miss(LvPoke *p)
{
  if (p->misses < 255) p->misses = (uint8_t)(p->misses + 1);
}

void lv_poke_init(LvPoke *p, uint64_t now_ms)
{
  p->next_poke_ms = now_ms + LV_POKE_INTERVAL_MS;
  p->deadline_ms  = 0;
  p->awaiting     = false;
  p->misses       = 0;
}

LvAction lv_poke_tick(LvPoke *p, uint64_t now_ms, bool link_ready)
{
  if (p->awaiting && now_ms >= p->deadline_ms) {   /* unanswered in time */
    p->awaiting = false;
    miss(p);
  }
  if (now_ms < p->next_poke_ms) return LV_NOTHING;

  p->next_poke_ms = now_ms + LV_POKE_INTERVAL_MS;  /* the cadence runs regardless */
  if (p->awaiting) {                               /* defensive: still open at the next due time */
    p->awaiting = false;
    miss(p);
  }
  if (!link_ready) {                               /* cannot even ask: that is a miss too */
    miss(p);
    return LV_NOTHING;
  }
  p->awaiting    = true;
  p->deadline_ms = now_ms + LV_ANSWER_TIMEOUT_MS;
  return LV_SEND_POKE;
}

void lv_poke_answered(LvPoke *p)
{
  p->awaiting = false;
  p->misses   = 0;
}

void lv_poke_link_up(LvPoke *p, uint64_t now_ms)
{
  if (!p->awaiting && p->next_poke_ms > now_ms) p->next_poke_ms = now_ms;
}

void lv_poke_align(LvPoke *p, const LvClock *c, uint64_t now_ms, uint64_t reading_epoch_ms)
{
  if (!c || !c->synced) return;
  /* Local ms at which the reading was taken. Signed: it may predate boot. */
  int64_t reading_local = (int64_t)c->local_at_sync +
                          ((int64_t)reading_epoch_ms - (int64_t)c->epoch_at_sync);
  int64_t target = reading_local + (int64_t)LV_POKE_INTERVAL_MS + (int64_t)LV_ALIGN_LAG_MS;
  int64_t lo     = (int64_t)now_ms + (int64_t)LV_MIN_POKE_GAP_MS;
  int64_t hi     = (int64_t)now_ms + (int64_t)LV_POKE_INTERVAL_MS;
  if (target < lo) target = lo;
  if (target > hi) target = hi;
  p->next_poke_ms = (uint64_t)target;
}

bool lv_blind(const LvPoke *p)
{
  return p->misses >= LV_MISS_ALERT_COUNT;
}

/* ---- alert selection ---- */

LvData lv_data_state(const LvClock *c, uint64_t now_ms, bool have_reading,
                     uint64_t reading_epoch_ms, uint64_t stale_ms, uint64_t boot_ms)
{
  if (have_reading)
    return lv_judge(c, now_ms, reading_epoch_ms, stale_ms) == LV_FRESH
           ? LV_DATA_CURRENT : LV_DATA_NOT_CURRENT;
  return (now_ms > boot_ms && now_ms - boot_ms > stale_ms) ? LV_DATA_NOT_CURRENT : LV_DATA_GRACE;
}

LvAlert lv_select(LvData data, int32_t sgv, int32_t low, int32_t high, bool blind)
{
  if (data == LV_DATA_CURRENT) {
    if (low  != RC_UNSET && sgv < low)  return LV_ALERT_LOW;
    if (high != RC_UNSET && sgv > high) return LV_ALERT_HIGH;
  }
  if (blind)                       return LV_ALERT_MISS;
  if (data == LV_DATA_NOT_CURRENT) return LV_ALERT_STALE;
  return LV_ALERT_NONE;
}

const char *lv_alert_name(LvAlert a)
{
  switch (a) {
    case LV_ALERT_LOW:   return "low";
    case LV_ALERT_HIGH:  return "high";
    case LV_ALERT_STALE: return "stale";
    case LV_ALERT_MISS:  return "miss";
    default:             return "none";
  }
}
