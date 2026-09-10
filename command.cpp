#include "command.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAX_TOKENS 8
#define TOKEN_MAX  40

typedef struct {
  int  count;
  char t[MAX_TOKENS][TOKEN_MAX];
} Tokens;

/* Split on runs of spaces. Over-long tokens and extra tokens are a parse
 * failure rather than a silent truncation. */
static bool tokenize(const char *line, Tokens *out)
{
  out->count = 0;
  const char *p = line;

  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    if (out->count >= MAX_TOKENS) return false;

    const char *start = p;
    while (*p && *p != ' ') p++;
    size_t len = (size_t)(p - start);
    if (len >= TOKEN_MAX) return false;

    memcpy(out->t[out->count], start, len);
    out->t[out->count][len] = '\0';
    out->count++;
  }
  return true;
}

static int resp_fmt(char *resp, size_t size, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(resp, size, fmt, ap);
  va_end(ap);
  if (n < 0) { resp[0] = '\0'; return 0; }
  if ((size_t)n >= size) n = (int)size - 1;   /* truncated but terminated */
  return n;
}

static int err(char *resp, size_t size, const char *verb, const char *why)
{
  return resp_fmt(resp, size, "#ERR %s %s", verb, why);
}

static bool scan_int(const char *s, int32_t *out)
{
  if (!*s) return false;
  bool neg = false;
  const char *p = s;
  if (*p == '-') { neg = true; p++; if (!*p) return false; }
  int64_t acc = 0;
  for (; *p; p++) {
    if (*p < '0' || *p > '9') return false;
    acc = acc * 10 + (*p - '0');
    if (acc > 2147483647LL) return false;
  }
  *out = (int32_t)(neg ? -acc : acc);
  return true;
}

int cmd_is_command(const char *line)
{
  return line && line[0] == CMD_PREFIX;
}

int cmd_execute(const char *line, RingConfig *cfg, const CmdHost *host,
                char *resp, size_t resp_size)
{
  if (!resp || resp_size == 0) return 0;
  resp[0] = '\0';
  if (!line || !cfg)              return err(resp, resp_size, "?", "internal");
  if (line[0] != CMD_PREFIX)      return err(resp, resp_size, "?", "not a command");

  Tokens tk;
  if (!tokenize(line + 1, &tk))   return err(resp, resp_size, "?", "malformed");
  if (tk.count == 0)              return err(resp, resp_size, "?", "empty");

  const char *verb = tk.t[0];

  /* ---------------- VER ---------------- */
  if (!strcmp(verb, "VER")) {
    return resp_fmt(resp, resp_size, "#VER glucose-ring %s proto=%d schema=%d",
                    FW_VERSION, PROTO_VERSION, RC_SCHEMA_VERSION);
  }

  /* ---------------- GET ---------------- */
  if (!strcmp(verb, "GET")) {
    if (tk.count < 2) return err(resp, resp_size, "GET", "missing key");
    const char *key = tk.t[1];

    if (!strcmp(key, "THRESH")) {
      char lo[16], hi[16];
      if (cfg->low_mg_dl  == RC_UNSET) snprintf(lo, sizeof(lo), "unset");
      else                             snprintf(lo, sizeof(lo), "%ld", (long)cfg->low_mg_dl);
      if (cfg->high_mg_dl == RC_UNSET) snprintf(hi, sizeof(hi), "unset");
      else                             snprintf(hi, sizeof(hi), "%ld", (long)cfg->high_mg_dl);
      return resp_fmt(resp, resp_size, "#THRESH low=%s high=%s", lo, hi);
    }
    if (!strcmp(key, "STALE"))
      return resp_fmt(resp, resp_size, "#STALE %u", (unsigned)cfg->stale_minutes);
    if (!strcmp(key, "REPEAT"))
      return resp_fmt(resp, resp_size, "#REPEAT %u", (unsigned)cfg->repeat_minutes);
    if (!strcmp(key, "INTENSITY"))
      return resp_fmt(resp, resp_size, "#INTENSITY %u", (unsigned)cfg->intensity);
    if (!strcmp(key, "STALEREPEATS"))
      return resp_fmt(resp, resp_size, "#STALEREPEATS %u", (unsigned)cfg->stale_max_repeats);

    if (!strcmp(key, "PATTERN")) {
      if (tk.count < 3) return err(resp, resp_size, "GET", "missing pattern name");
      const RcPattern *p = rc_pattern_by_name_const(cfg, tk.t[2]);
      if (!p) return err(resp, resp_size, "GET", "unknown pattern");
      char pb[80];
      if (rc_pattern_format(p, pb, sizeof(pb)) < 0)
        return err(resp, resp_size, "GET", "format");
      return resp_fmt(resp, resp_size, "#PATTERN %s %s", tk.t[2], pb);
    }

    if (!strcmp(key, "BATT")) {
      if (!host || !host->batt_mv) return err(resp, resp_size, "GET", "unsupported");
      unsigned mv  = host->batt_mv(host->ctx);
      unsigned pct = host->batt_pct ? host->batt_pct(host->ctx) : 0;
      int chg = host->charging ? host->charging(host->ctx) : -1;
      return resp_fmt(resp, resp_size, "#BATT mv=%u pct=%u chg=%d", mv, pct, chg);
    }

    if (!strcmp(key, "STATUS")) {
      if (!host) return err(resp, resp_size, "GET", "unsupported");
      int have = host->have_reading ? host->have_reading(host->ctx) : 0;
      const char *alert = host->alert_name ? host->alert_name(host->ctx) : "unknown";
      unsigned age = host->reading_age_s ? host->reading_age_s(host->ctx) : 0;
      int stale = host->is_stale ? host->is_stale(host->ctx) : -1;
      unsigned up = host->uptime_s ? host->uptime_s(host->ctx) : 0;
      if (!have)
        return resp_fmt(resp, resp_size,
                        "#STATUS sgv=none dir= age_s=0 stale=%d alert=%s up_s=%u",
                        stale, alert, up);
      return resp_fmt(resp, resp_size,
                      "#STATUS sgv=%ld dir=%s age_s=%u stale=%d alert=%s up_s=%u",
                      (long)host->sgv(host->ctx),
                      host->direction ? host->direction(host->ctx) : "",
                      age, stale, alert, up);
    }

    return err(resp, resp_size, "GET", "unknown key");
  }

  /* ---------------- SET ---------------- */
  if (!strcmp(verb, "SET")) {
    if (tk.count < 2) return err(resp, resp_size, "SET", "missing key");
    const char *key = tk.t[1];

    /* Mutate a scratch copy, validate, then commit. A rejected SET must
       never leave the live config half-modified. */
    RingConfig next = *cfg;

    if (!strcmp(key, "THRESH")) {
      if (tk.count < 4) return err(resp, resp_size, "SET", "usage: !SET THRESH <low|unset> <high|unset>");
      int32_t lo, hi;
      if (!strcmp(tk.t[2], "unset")) lo = RC_UNSET;
      else if (!scan_int(tk.t[2], &lo)) return err(resp, resp_size, "SET", "bad low");
      if (!strcmp(tk.t[3], "unset")) hi = RC_UNSET;
      else if (!scan_int(tk.t[3], &hi)) return err(resp, resp_size, "SET", "bad high");
      next.low_mg_dl = lo;
      next.high_mg_dl = hi;
    }
    else if (!strcmp(key, "STALE")) {
      if (tk.count < 3) return err(resp, resp_size, "SET", "missing value");
      int32_t v; if (!scan_int(tk.t[2], &v) || v < 0 || v > 65535)
        return err(resp, resp_size, "SET", "bad value");
      next.stale_minutes = (uint16_t)v;
    }
    else if (!strcmp(key, "REPEAT")) {
      if (tk.count < 3) return err(resp, resp_size, "SET", "missing value");
      int32_t v; if (!scan_int(tk.t[2], &v) || v < 0 || v > 65535)
        return err(resp, resp_size, "SET", "bad value");
      next.repeat_minutes = (uint16_t)v;
    }
    else if (!strcmp(key, "INTENSITY")) {
      if (tk.count < 3) return err(resp, resp_size, "SET", "missing value");
      int32_t v; if (!scan_int(tk.t[2], &v) || v < 0 || v > 255)
        return err(resp, resp_size, "SET", "bad value");
      next.intensity = (uint8_t)v;
    }
    else if (!strcmp(key, "STALEREPEATS")) {
      if (tk.count < 3) return err(resp, resp_size, "SET", "missing value");
      int32_t v; if (!scan_int(tk.t[2], &v) || v < 0 || v > RC_STALE_REPEATS_MAX)
        return err(resp, resp_size, "SET", "bad value");
      next.stale_max_repeats = (uint8_t)v;   /* 0 = never stop repeating */
    }
    else if (!strcmp(key, "PATTERN")) {
      if (tk.count < 4) return err(resp, resp_size, "SET", "usage: !SET PATTERN <name> <on/off,...>");
      RcPattern *dst = rc_pattern_by_name(&next, tk.t[2]);
      if (!dst) return err(resp, resp_size, "SET", "unknown pattern");
      RcPattern parsed;
      if (!rc_pattern_parse(tk.t[3], &parsed))
        return err(resp, resp_size, "SET", "bad pattern");
      *dst = parsed;
    }
    else {
      return err(resp, resp_size, "SET", "unknown key");
    }

    const char *why = NULL;
    if (!rc_validate(&next, &why))
      return err(resp, resp_size, "SET", why ? why : "invalid");

    *cfg = next;
    if (host && host->mark_dirty) host->mark_dirty(host->ctx);
    return resp_fmt(resp, resp_size, "#OK SET %s", key);
  }

  /* ---------------- BUZZ ---------------- */
  if (!strcmp(verb, "BUZZ")) {
    if (tk.count < 2) return err(resp, resp_size, "BUZZ", "missing pattern");
    if (!host || !host->buzz) return err(resp, resp_size, "BUZZ", "unsupported");
    if (!host->buzz(host->ctx, tk.t[1]))
      return err(resp, resp_size, "BUZZ", "unknown pattern");
    return resp_fmt(resp, resp_size, "#OK BUZZ %s", tk.t[1]);
  }

  /* ---------------- SAVE ---------------- */
  if (!strcmp(verb, "SAVE")) {
    if (!host || !host->save) return err(resp, resp_size, "SAVE", "unsupported");
    if (!host->save(host->ctx)) return err(resp, resp_size, "SAVE", "write failed");
    return resp_fmt(resp, resp_size, "#OK SAVE");
  }

  /* ---------------- RESET ---------------- */
  if (!strcmp(verb, "RESET")) {
    if (tk.count < 2 || strcmp(tk.t[1], "CONFIG"))
      return err(resp, resp_size, "RESET", "usage: !RESET CONFIG");
    rc_defaults(cfg);
    if (host && host->factory_reset) host->factory_reset(host->ctx);
    else if (host && host->mark_dirty) host->mark_dirty(host->ctx);
    return resp_fmt(resp, resp_size, "#OK RESET CONFIG");
  }

  return err(resp, resp_size, "?", "unknown verb");
}
