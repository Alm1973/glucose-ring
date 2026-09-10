#include "ring_config.h"
#include <stdio.h>
#include <string.h>

/* ---- small strict scanners (no strtol: it accepts whitespace and '+') ---- */

static bool scan_i32(const char *s, const char *end, int32_t *out)
{
  if (s >= end) return false;
  bool neg = false;
  if (*s == '-') { neg = true; s++; if (s >= end) return false; }

  int64_t acc = 0;
  for (const char *p = s; p < end; p++) {
    if (*p < '0' || *p > '9') return false;
    acc = acc * 10 + (*p - '0');
    if (acc > 2147483647LL) return false;   /* also caps the negative side */
  }
  *out = (int32_t)(neg ? -acc : acc);
  return true;
}

static bool scan_u16(const char *s, const char *end, uint16_t *out)
{
  int32_t v;
  if (!scan_i32(s, end, &v)) return false;
  if (v < 0 || v > 65535) return false;
  *out = (uint16_t)v;
  return true;
}

static const char *line_end(const char *p)
{
  while (*p && *p != '\n' && *p != '\r') p++;
  return p;
}

/* ---- patterns ---- */

bool rc_pattern_parse(const char *s, RcPattern *out)
{
  if (!s || !out) return false;

  /* Zero the whole struct, not just count: unused pulse slots must be
     canonical so two structurally-equal configs compare equal, and so no
     stack garbage ever rides along in a persisted struct. */
  RcPattern p;
  memset(&p, 0, sizeof(p));

  const char *cur = s;
  while (*cur) {
    if (p.count >= RC_PATTERN_MAX_PULSES) return false;

    const char *slash = cur;
    while (*slash && *slash != '/' && *slash != ',') slash++;
    if (*slash != '/') return false;                 /* each pulse needs on/off */

    const char *comma = slash + 1;
    while (*comma && *comma != ',') comma++;

    uint16_t on, off;
    if (!scan_u16(cur, slash, &on))        return false;
    if (!scan_u16(slash + 1, comma, &off)) return false;
    if (on  < RC_PULSE_ON_MIN || on > RC_PULSE_ON_MAX) return false;
    if (off > RC_PULSE_OFF_MAX)                        return false;

    p.pulses[p.count].on_ms  = on;
    p.pulses[p.count].off_ms = off;
    p.count++;

    if (*comma == ',') {
      cur = comma + 1;
      if (*cur == '\0') return false;                 /* trailing comma */
    } else {
      cur = comma;                                    /* at NUL -> done */
    }
  }

  if (p.count == 0) return false;
  *out = p;
  return true;
}

int rc_pattern_format(const RcPattern *p, char *buf, size_t bufsize)
{
  if (!p || !buf || bufsize == 0) return -1;
  size_t off = 0;
  for (uint8_t i = 0; i < p->count; i++) {
    int n = snprintf(buf + off, bufsize - off, "%s%u/%u",
                     i ? "," : "",
                     (unsigned)p->pulses[i].on_ms,
                     (unsigned)p->pulses[i].off_ms);
    if (n < 0 || (size_t)n >= bufsize - off) return -1;
    off += (size_t)n;
  }
  return (int)off;
}

RcPattern *rc_pattern_by_name(RingConfig *c, const char *name)
{
  if (!c || !name) return NULL;
  if (!strcmp(name, "low"))   return &c->pat_low;
  if (!strcmp(name, "high"))  return &c->pat_high;
  if (!strcmp(name, "stale")) return &c->pat_stale;
  return NULL;
}

const RcPattern *rc_pattern_by_name_const(const RingConfig *c, const char *name)
{
  return rc_pattern_by_name((RingConfig *)c, name);
}

/* ---- defaults ---- */

void rc_defaults(RingConfig *c)
{
  if (!c) return;
  memset(c, 0, sizeof(*c));

  c->version        = RC_SCHEMA_VERSION;
  c->low_mg_dl      = RC_UNSET;   /* deliberately unset -- see header */
  c->high_mg_dl     = RC_UNSET;
  c->stale_minutes  = 15;
  c->repeat_minutes = 5;
  c->intensity      = 100;        /* full drive; lower it if it is too strong */
  c->stale_max_repeats = 6;       /* ~30 min at the default 5-minute repeat */

  rc_pattern_parse("400/200,400/200,400/0", &c->pat_low);
  rc_pattern_parse("120/150,120/0",         &c->pat_high);
  rc_pattern_parse("60/120,60/0",           &c->pat_stale);
}

/* ---- validation ---- */

static bool valid_threshold(int32_t v)
{
  return v == RC_UNSET || (v >= RC_SGV_MIN && v <= RC_SGV_MAX);
}

static bool valid_pattern(const RcPattern *p)
{
  if (p->count == 0 || p->count > RC_PATTERN_MAX_PULSES) return false;
  for (uint8_t i = 0; i < p->count; i++) {
    if (p->pulses[i].on_ms < RC_PULSE_ON_MIN)  return false;
    if (p->pulses[i].on_ms > RC_PULSE_ON_MAX)  return false;
    if (p->pulses[i].off_ms > RC_PULSE_OFF_MAX) return false;
  }
  return true;
}

bool rc_validate(const RingConfig *c, const char **why)
{
  const char *reason = NULL;

  if (!c)                              reason = "null";
  else if (c->version > RC_SCHEMA_VERSION) reason = "schema newer than firmware";
  else if (!valid_threshold(c->low_mg_dl))  reason = "low out of range";
  else if (!valid_threshold(c->high_mg_dl)) reason = "high out of range";
  else if (c->low_mg_dl  != RC_UNSET &&
           c->high_mg_dl != RC_UNSET &&
           c->low_mg_dl >= c->high_mg_dl)   reason = "low must be below high";
  else if (c->stale_minutes < RC_STALE_MIN_MIN ||
           c->stale_minutes > RC_STALE_MIN_MAX)   reason = "stale out of range";
  else if (c->repeat_minutes < RC_REPEAT_MIN_MIN ||
           c->repeat_minutes > RC_REPEAT_MIN_MAX) reason = "repeat out of range";
  else if (c->intensity < RC_INTENSITY_MIN ||
           c->intensity > RC_INTENSITY_MAX)       reason = "intensity out of range";
  else if (!valid_pattern(&c->pat_low))   reason = "low pattern invalid";
  else if (!valid_pattern(&c->pat_high))  reason = "high pattern invalid";
  else if (!valid_pattern(&c->pat_stale)) reason = "stale pattern invalid";

  if (why) *why = reason;
  return reason == NULL;
}

/* ---- serialise ---- */

int rc_serialize(const RingConfig *c, char *buf, size_t bufsize)
{
  if (!c || !buf || bufsize == 0) return -1;

  char pl[80], ph[80], ps[80];
  if (rc_pattern_format(&c->pat_low,   pl, sizeof(pl)) < 0) return -1;
  if (rc_pattern_format(&c->pat_high,  ph, sizeof(ph)) < 0) return -1;
  if (rc_pattern_format(&c->pat_stale, ps, sizeof(ps)) < 0) return -1;

  int n = snprintf(buf, bufsize,
                   "version=%u\n"
                   "low=%ld\n"
                   "high=%ld\n"
                   "stale=%u\n"
                   "repeat=%u\n"
                   "intensity=%u\n"
                   "stale_repeats=%u\n"
                   "pat_low=%s\n"
                   "pat_high=%s\n"
                   "pat_stale=%s\n",
                   (unsigned)c->version,
                   (long)c->low_mg_dl,
                   (long)c->high_mg_dl,
                   (unsigned)c->stale_minutes,
                   (unsigned)c->repeat_minutes,
                   (unsigned)c->intensity,
                   (unsigned)c->stale_max_repeats,
                   pl, ph, ps);

  if (n < 0 || (size_t)n >= bufsize) return -1;
  return n;
}

/* ---- deserialise ---- */

static bool key_is(const char *k, const char *k_end, const char *want)
{
  size_t len = (size_t)(k_end - k);
  return strlen(want) == len && memcmp(k, want, len) == 0;
}

bool rc_deserialize(const char *text, RingConfig *out, const char **why)
{
  if (!text || !out) { if (why) *why = "null"; return false; }

  RingConfig c;
  rc_defaults(&c);              /* missing keys keep defaults */

  const char *p = text;
  while (*p) {
    const char *eol = line_end(p);

    /* skip blank lines and comments */
    if (eol > p && *p != '#') {
      const char *eq = p;
      while (eq < eol && *eq != '=') eq++;

      if (eq < eol) {
        const char *k = p, *k_end = eq;
        const char *v = eq + 1, *v_end = eol;
        char vbuf[96];
        size_t vlen = (size_t)(v_end - v);

        if (vlen < sizeof(vbuf)) {
          memcpy(vbuf, v, vlen);
          vbuf[vlen] = '\0';

          if      (key_is(k, k_end, "version"))   { if (!scan_u16(v, v_end, &c.version))        { if (why) *why = "bad version";   return false; } }
          else if (key_is(k, k_end, "low"))       { if (!scan_i32(v, v_end, &c.low_mg_dl))      { if (why) *why = "bad low";       return false; } }
          else if (key_is(k, k_end, "high"))      { if (!scan_i32(v, v_end, &c.high_mg_dl))     { if (why) *why = "bad high";      return false; } }
          else if (key_is(k, k_end, "stale"))     { if (!scan_u16(v, v_end, &c.stale_minutes))  { if (why) *why = "bad stale";     return false; } }
          else if (key_is(k, k_end, "repeat"))    { if (!scan_u16(v, v_end, &c.repeat_minutes)) { if (why) *why = "bad repeat";    return false; } }
          else if (key_is(k, k_end, "intensity")) { uint16_t t; if (!scan_u16(v, v_end, &t) || t > 255) { if (why) *why = "bad intensity"; return false; } c.intensity = (uint8_t)t; }
          else if (key_is(k, k_end, "stale_repeats")) { uint16_t t; if (!scan_u16(v, v_end, &t) || t > RC_STALE_REPEATS_MAX) { if (why) *why = "bad stale_repeats"; return false; } c.stale_max_repeats = (uint8_t)t; }
          else if (key_is(k, k_end, "pat_low"))   { if (!rc_pattern_parse(vbuf, &c.pat_low))    { if (why) *why = "bad pat_low";   return false; } }
          else if (key_is(k, k_end, "pat_high"))  { if (!rc_pattern_parse(vbuf, &c.pat_high))   { if (why) *why = "bad pat_high";  return false; } }
          else if (key_is(k, k_end, "pat_stale")) { if (!rc_pattern_parse(vbuf, &c.pat_stale))  { if (why) *why = "bad pat_stale"; return false; } }
          /* unknown key: ignored on purpose (forward compatibility) */
        } else {
          if (why) *why = "value too long";
          return false;
        }
      }
    }

    p = eol;
    while (*p == '\n' || *p == '\r') p++;
  }

  if (!rc_validate(&c, why)) return false;

  *out = c;
  if (why) *why = NULL;
  return true;
}
