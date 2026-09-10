#include "reading_parser.h"

const RpLimits RP_DEFAULT_LIMITS = {
  20,               /* sgv_min      */
  600,              /* sgv_max      */
  1600000000000ULL  /* ~2020-09-13; anything older is a bad timestamp */
};

/*
 * Strict unsigned decimal parse over [s, end).
 * Rejects empty, any non-digit (including sign and whitespace), and
 * anything that would overflow uint64_t.
 */
static bool parse_u64_strict(const char *s, const char *end, uint64_t *out)
{
  if (s >= end) return false;

  uint64_t acc = 0;
  for (const char *p = s; p < end; p++) {
    if (*p < '0' || *p > '9') return false;
    uint64_t digit = (uint64_t)(*p - '0');
    /* acc*10 + digit must not wrap */
    if (acc > (UINT64_MAX - digit) / 10ULL) return false;
    acc = acc * 10ULL + digit;
  }
  *out = acc;
  return true;
}

RpResult rp_parse(const char *line, const RpLimits *limits, RpReading *out)
{
  if (!line || !limits || !out) return RP_ERR_NULL;
  if (line[0] == '\0')          return RP_ERR_EMPTY;

  /* --- split into exactly three fields, without mutating `line` --- */
  const char *c1 = NULL;
  const char *c2 = NULL;
  for (const char *p = line; *p; p++) {
    if (*p == ',') {
      if      (!c1) c1 = p;
      else if (!c2) c2 = p;
      else          return RP_ERR_FIELD_COUNT;  /* fourth field */
    }
  }
  if (!c1 || !c2) return RP_ERR_FIELD_COUNT;

  const char *f1 = line,   *f1_end = c1;
  const char *f2 = c1 + 1, *f2_end = c2;
  const char *f3 = c2 + 1, *f3_end = f3;
  while (*f3_end) f3_end++;

  /* --- field 1: sgv --- */
  uint64_t sgv_raw = 0;
  if (!parse_u64_strict(f1, f1_end, &sgv_raw)) return RP_ERR_SGV_FORMAT;
  /* Compare in 64-bit before narrowing so a huge value is a RANGE error,
     not a truncated in-range one. */
  if (sgv_raw > (uint64_t)INT32_MAX)           return RP_ERR_SGV_RANGE;
  int32_t sgv = (int32_t)sgv_raw;
  if (sgv < limits->sgv_min || sgv > limits->sgv_max) return RP_ERR_SGV_RANGE;

  /* --- field 2: direction (may be empty; spaces allowed) --- */
  size_t dlen = (size_t)(f2_end - f2);
  if (dlen >= RP_DIRECTION_MAX) return RP_ERR_DIRECTION;
  for (size_t i = 0; i < dlen; i++) {
    unsigned char ch = (unsigned char)f2[i];
    if (ch < 0x20 || ch > 0x7E) return RP_ERR_DIRECTION;
  }

  /* --- field 3: epoch ms --- */
  uint64_t epoch = 0;
  if (!parse_u64_strict(f3, f3_end, &epoch)) return RP_ERR_EPOCH_FORMAT;
  if (epoch < limits->epoch_ms_min)          return RP_ERR_EPOCH_RANGE;

  /* --- commit (only now that everything validated) --- */
  out->sgv = sgv;
  for (size_t i = 0; i < dlen; i++) out->direction[i] = f2[i];
  out->direction[dlen] = '\0';
  out->epoch_ms = epoch;
  return RP_OK;
}

const char *rp_result_name(RpResult r)
{
  switch (r) {
    case RP_OK:              return "OK";
    case RP_ERR_NULL:        return "ERR_NULL";
    case RP_ERR_EMPTY:       return "ERR_EMPTY";
    case RP_ERR_FIELD_COUNT: return "ERR_FIELD_COUNT";
    case RP_ERR_SGV_FORMAT:  return "ERR_SGV_FORMAT";
    case RP_ERR_SGV_RANGE:   return "ERR_SGV_RANGE";
    case RP_ERR_DIRECTION:   return "ERR_DIRECTION";
    case RP_ERR_EPOCH_FORMAT:return "ERR_EPOCH_FORMAT";
    case RP_ERR_EPOCH_RANGE: return "ERR_EPOCH_RANGE";
    default:                 return "ERR_UNKNOWN";
  }
}
