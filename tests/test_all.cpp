/*
 * Host test harness. Plain C++17, clang, no Arduino.
 *
 *   ./tests/run_tests.sh
 *
 * Links the SAME reading_parser.cpp / ring_config.cpp / command.cpp that the
 * firmware compiles, so these tests exercise shipping code, not a copy.
 */
#include "../reading_parser.h"
#include "../ring_config.h"
#include "../command.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0, g_fail = 0;
static const char *g_section = "";

static void section(const char *s) { g_section = s; printf("\n-- %s\n", s); }

static void ok(bool cond, const std::string &what)
{
  if (cond) { g_pass++; }
  else      { g_fail++; printf("  FAIL [%s] %s\n", g_section, what.c_str()); }
}

/* Render bytes printably so failures involving control chars are readable. */
static std::string show(const char *s)
{
  std::string r = "\"";
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    char b[8];
    if (*p >= 0x20 && *p <= 0x7E) { b[0] = (char)*p; b[1] = '\0'; }
    else snprintf(b, sizeof(b), "\\x%02X", *p);
    r += b;
  }
  return r + "\"";
}

/* ================= reading parser ================= */

static void expect_parse(const char *line, RpResult want)
{
  RpReading r;
  RpResult got = rp_parse(line, &RP_DEFAULT_LIMITS, &r);
  ok(got == want, show(line) + " -> " + rp_result_name(got) +
                  " (want " + rp_result_name(want) + ")");
}

static void expect_ok(const char *line, int32_t sgv, const char *dir, uint64_t epoch)
{
  RpReading r;
  RpResult got = rp_parse(line, &RP_DEFAULT_LIMITS, &r);
  if (got != RP_OK) {
    ok(false, show(line) + " -> " + rp_result_name(got) + " (want OK)");
    return;
  }
  ok(r.sgv == sgv, show(line) + " sgv=" + std::to_string(r.sgv));
  ok(strcmp(r.direction, dir) == 0, show(line) + " dir=" + show(r.direction));
  ok(r.epoch_ms == epoch, show(line) + " epoch");
}

static void test_parser_valid()
{
  section("parser: valid input");
  expect_ok("112,Flat,1789238400000", 112, "Flat", 1789238400000ULL);
  expect_ok("20,,1600000000000", 20, "", 1600000000000ULL);          /* min sgv, empty dir */
  expect_ok("600,Flat,1600000000000", 600, "Flat", 1600000000000ULL); /* max sgv */
  expect_ok("070,Flat,1789238400000", 70, "Flat", 1789238400000ULL);  /* leading zeros */
  /* Nightscout directions containing spaces must survive intact. */
  expect_ok("95,RATE OUT OF RANGE,1789238400000", 95, "RATE OUT OF RANGE", 1789238400000ULL);
  expect_ok("95,NOT COMPUTABLE,1789238400000", 95, "NOT COMPUTABLE", 1789238400000ULL);
  expect_ok("300,DoubleUp,1789238400001", 300, "DoubleUp", 1789238400001ULL);

  /* direction of exactly RP_DIRECTION_MAX-1 chars is the longest legal one */
  {
    std::string d(RP_DIRECTION_MAX - 1, 'x');
    std::string line = "100," + d + ",1789238400000";
    expect_ok(line.c_str(), 100, d.c_str(), 1789238400000ULL);
  }
}

static void test_parser_malformed()
{
  section("parser: malformed");
  expect_parse("", RP_ERR_EMPTY);
  expect_parse("112", RP_ERR_FIELD_COUNT);
  expect_parse("112,Flat", RP_ERR_FIELD_COUNT);
  expect_parse("112,Flat,1789238400000,extra", RP_ERR_FIELD_COUNT);
  expect_parse("112,Flat,1789238400000,", RP_ERR_FIELD_COUNT);
  expect_parse(",Flat,1789238400000", RP_ERR_SGV_FORMAT);       /* empty sgv   */
  expect_parse("112,Flat,", RP_ERR_EPOCH_FORMAT);               /* empty epoch */
  expect_parse("11a,Flat,1789238400000", RP_ERR_SGV_FORMAT);
  expect_parse("112.5,Flat,1789238400000", RP_ERR_SGV_FORMAT);
  expect_parse("+112,Flat,1789238400000", RP_ERR_SGV_FORMAT);   /* sign rejected  */
  expect_parse("-5,Flat,1789238400000", RP_ERR_SGV_FORMAT);
  expect_parse(" 112,Flat,1789238400000", RP_ERR_SGV_FORMAT);   /* leading space  */
  expect_parse("112 ,Flat,1789238400000", RP_ERR_SGV_FORMAT);   /* trailing space */
  expect_parse("0x70,Flat,1789238400000", RP_ERR_SGV_FORMAT);
  expect_parse("112,Flat,17892384000x0", RP_ERR_EPOCH_FORMAT);
  expect_parse("112,Flat,1.79e12", RP_ERR_EPOCH_FORMAT);
  expect_parse("112,Flat,-1789238400000", RP_ERR_EPOCH_FORMAT);
  expect_parse("112,Flat, 1789238400000", RP_ERR_EPOCH_FORMAT);
}

static void test_parser_truncated()
{
  section("parser: truncated");
  /* Every prefix of a good line must be rejected, never half-accepted. */
  const char *full = "112,Flat,1789238400000";
  for (size_t n = 1; n < strlen(full); n++) {
    std::string p(full, n);
    RpReading r;
    RpResult got = rp_parse(p.c_str(), &RP_DEFAULT_LIMITS, &r);
    ok(got != RP_OK, "prefix " + show(p.c_str()) + " must not parse");
  }
  expect_parse("1", RP_ERR_FIELD_COUNT);
  expect_parse("112,", RP_ERR_FIELD_COUNT);
  expect_parse("112,,", RP_ERR_EPOCH_FORMAT);

  /* An embedded NUL truncates the line; the tail must not be reachable. */
  char buf[] = "112,Flat,1789238400000";
  buf[3] = '\0';
  expect_parse(buf, RP_ERR_FIELD_COUNT);
}

static void test_parser_range()
{
  section("parser: out of range");
  expect_parse("19,Flat,1789238400000", RP_ERR_SGV_RANGE);
  expect_parse("0,Flat,1789238400000", RP_ERR_SGV_RANGE);
  expect_parse("601,Flat,1789238400000", RP_ERR_SGV_RANGE);
  expect_parse("5000000000,Flat,1789238400000", RP_ERR_SGV_RANGE);  /* > INT32_MAX */
  expect_parse("112,Flat,1599999999999", RP_ERR_EPOCH_RANGE);       /* 1ms below floor */
  expect_parse("112,Flat,0", RP_ERR_EPOCH_RANGE);
  expect_parse("112,Flat,1600000000000", RP_OK);                    /* exactly the floor */

  /* u64 overflow in either numeric field is a format error, not a wrap. */
  expect_parse("99999999999999999999999,Flat,1789238400000", RP_ERR_SGV_FORMAT);
  expect_parse("112,Flat,99999999999999999999999", RP_ERR_EPOCH_FORMAT);

  /* custom limits are honoured */
  {
    RpLimits lim = { 50, 150, 1000ULL };
    RpReading r;
    ok(rp_parse("40,Flat,2000", &lim, &r) == RP_ERR_SGV_RANGE, "custom lim low");
    ok(rp_parse("160,Flat,2000", &lim, &r) == RP_ERR_SGV_RANGE, "custom lim high");
    ok(rp_parse("100,Flat,2000", &lim, &r) == RP_OK, "custom lim ok");
  }
}

static void test_parser_garbage()
{
  section("parser: garbage bytes / direction hygiene");
  expect_parse("\xFF\xFE\xFD", RP_ERR_FIELD_COUNT);
  expect_parse("\x01,\x02,\x03", RP_ERR_SGV_FORMAT);
  expect_parse("112,Fl\x01at,1789238400000", RP_ERR_DIRECTION);   /* control byte */
  expect_parse("112,Fl\tat,1789238400000", RP_ERR_DIRECTION);     /* tab          */
  expect_parse("112,Fl\xFF" "at,1789238400000", RP_ERR_DIRECTION);   /* high byte    */
  expect_parse("112,Fl\x7F" "at,1789238400000", RP_ERR_DIRECTION);   /* DEL          */

  /* over-long direction is an error, never a silent truncation */
  {
    std::string d(RP_DIRECTION_MAX, 'x');
    std::string line = "100," + d + ",1789238400000";
    expect_parse(line.c_str(), RP_ERR_DIRECTION);
  }

  /* a long run of commas must not walk off the end */
  {
    std::string s(200, ',');
    expect_parse(s.c_str(), RP_ERR_FIELD_COUNT);
  }
  /* very long numeric field */
  {
    std::string s(500, '9');
    std::string line = s + ",Flat,1789238400000";
    expect_parse(line.c_str(), RP_ERR_SGV_FORMAT);
  }
}

static void test_parser_contract()
{
  section("parser: API contract");
  RpReading r;
  ok(rp_parse(NULL, &RP_DEFAULT_LIMITS, &r) == RP_ERR_NULL, "null line");
  ok(rp_parse("1,a,1600000000000", NULL, &r) == RP_ERR_NULL, "null limits");
  ok(rp_parse("1,a,1600000000000", &RP_DEFAULT_LIMITS, NULL) == RP_ERR_NULL, "null out");

  /* input must not be mutated (the old in-place parser wrote NULs into it) */
  char buf[] = "112,Flat,1789238400000";
  char copy[sizeof(buf)];
  memcpy(copy, buf, sizeof(buf));
  rp_parse(buf, &RP_DEFAULT_LIMITS, &r);
  ok(memcmp(buf, copy, sizeof(buf)) == 0, "input not mutated");

  /* `out` must be untouched when parsing fails */
  RpReading sentinel;
  memset(&sentinel, 0xAB, sizeof(sentinel));
  RpReading probe = sentinel;
  rp_parse("garbage", &RP_DEFAULT_LIMITS, &probe);
  ok(memcmp(&probe, &sentinel, sizeof(probe)) == 0, "out untouched on failure");

  /* direction is always NUL-terminated within bounds on success */
  rp_parse("112,Flat,1789238400000", &RP_DEFAULT_LIMITS, &r);
  bool terminated = false;
  for (size_t i = 0; i < RP_DIRECTION_MAX; i++) if (r.direction[i] == '\0') terminated = true;
  ok(terminated, "direction NUL-terminated");
}

/* ================= config ================= */

static void test_config()
{
  section("config: defaults and validation");
  RingConfig c;
  rc_defaults(&c);
  const char *why = NULL;
  ok(rc_validate(&c, &why), std::string("defaults valid: ") + (why ? why : ""));
  ok(c.low_mg_dl == RC_UNSET && c.high_mg_dl == RC_UNSET, "thresholds default to unset");
  ok(c.stale_minutes == 15, "default stale 15");
  ok(c.stale_max_repeats == 6, "default stale_max_repeats 6");
  {
    RingConfig t; rc_defaults(&t);
    t.stale_max_repeats = 0;
    ok(rc_validate(&t, NULL), "stale_max_repeats 0 (never stop) is legal");
    t.stale_max_repeats = 255;
    ok(rc_validate(&t, NULL), "stale_max_repeats 255 is legal");
  }

  struct { const char *name; void (*mut)(RingConfig *); } bad[] = {
    { "stale=0",        [](RingConfig *x){ x->stale_minutes = 0; } },
    { "stale=1441",     [](RingConfig *x){ x->stale_minutes = 1441; } },
    { "repeat=0",       [](RingConfig *x){ x->repeat_minutes = 0; } },
    { "intensity=0",    [](RingConfig *x){ x->intensity = 0; } },
    { "intensity=101",  [](RingConfig *x){ x->intensity = 101; } },
    { "low too small",  [](RingConfig *x){ x->low_mg_dl = 10; } },
    { "high too big",   [](RingConfig *x){ x->high_mg_dl = 700; } },
    { "low>=high",      [](RingConfig *x){ x->low_mg_dl = 200; x->high_mg_dl = 100; } },
    { "low==high",      [](RingConfig *x){ x->low_mg_dl = 100; x->high_mg_dl = 100; } },
    { "empty pattern",  [](RingConfig *x){ x->pat_low.count = 0; } },
    { "pattern count",  [](RingConfig *x){ x->pat_low.count = RC_PATTERN_MAX_PULSES + 1; } },
    { "pulse too long", [](RingConfig *x){ x->pat_low.pulses[0].on_ms = 9000; } },
    { "pulse too short",[](RingConfig *x){ x->pat_low.pulses[0].on_ms = 1; } },
    { "schema newer",   [](RingConfig *x){ x->version = RC_SCHEMA_VERSION + 1; } },
  };
  for (auto &b : bad) {
    RingConfig t; rc_defaults(&t); b.mut(&t);
    ok(!rc_validate(&t, NULL), std::string("reject ") + b.name);
  }
  /* both-unset must stay legal: it is the shipped state */
  {
    RingConfig t; rc_defaults(&t);
    t.low_mg_dl = RC_UNSET; t.high_mg_dl = 200;
    ok(rc_validate(&t, NULL), "low unset + high set is valid");
  }

  section("config: serialise round trip");
  {
    RingConfig a; rc_defaults(&a);
    a.low_mg_dl = 70; a.high_mg_dl = 200; a.stale_minutes = 20; a.intensity = 60;
    a.stale_max_repeats = 3;
    rc_pattern_parse("500/300,500/0", &a.pat_low);

    char buf[RC_SERIAL_MAX];
    int n = rc_serialize(&a, buf, sizeof(buf));
    ok(n > 0, "serialize ok");

    RingConfig b;
    const char *w = NULL;
    ok(rc_deserialize(buf, &b, &w), std::string("deserialize ok: ") + (w ? w : ""));
    ok(memcmp(&a, &b, sizeof(a)) == 0, "round trip is byte-identical");
  }
  {
    /* serialise must refuse to half-write into a short buffer */
    RingConfig a; rc_defaults(&a);
    char small[16];
    ok(rc_serialize(&a, small, sizeof(small)) == -1, "serialize rejects short buffer");
  }

  section("config: deserialise robustness");
  {
    RingConfig b;
    /* unknown keys ignored, missing keys defaulted */
    ok(rc_deserialize("version=1\nlow=70\nfuture_key=whatever\n", &b, NULL),
       "unknown key ignored");
    ok(b.low_mg_dl == 70, "known key applied");
    ok(b.stale_minutes == 15, "missing key keeps default");

    ok(rc_deserialize("", &b, NULL), "empty text -> defaults");
    ok(b.low_mg_dl == RC_UNSET, "empty text gives unset thresholds");

    ok(rc_deserialize("# comment\n\n\nlow=80\n", &b, NULL), "comments and blanks");
    ok(b.low_mg_dl == 80, "value after comments");
    ok(rc_deserialize("low=80\r\nhigh=200\r\n", &b, NULL), "CRLF tolerated");
    ok(b.high_mg_dl == 200, "CRLF value");

    ok(rc_deserialize("stale_repeats=0\n", &b, NULL), "stale_repeats=0 accepted");
    ok(b.stale_max_repeats == 0, "stale_repeats 0 applied");
    ok(rc_deserialize("stale_repeats=12\n", &b, NULL), "stale_repeats=12 accepted");
    ok(b.stale_max_repeats == 12, "stale_repeats 12 applied");
    ok(!rc_deserialize("stale_repeats=256\n", &b, NULL), "reject stale_repeats 256");
    ok(!rc_deserialize("stale_repeats=x\n", &b, NULL), "reject non-numeric stale_repeats");
    /* a v1 config predates the field and must load with the default */
    ok(rc_deserialize("version=1\nlow=70\nhigh=200\n", &b, NULL), "v1 config still loads");
    ok(b.stale_max_repeats == 6, "v1 config gets default stale_max_repeats");

    ok(!rc_deserialize("low=abc\n", &b, NULL), "reject non-numeric");
    ok(!rc_deserialize("stale=0\n", &b, NULL), "reject out-of-range on load");
    ok(!rc_deserialize("low=200\nhigh=100\n", &b, NULL), "reject inverted thresholds");
    ok(!rc_deserialize("version=99\n", &b, NULL), "reject newer schema");
    ok(!rc_deserialize("pat_low=nonsense\n", &b, NULL), "reject bad pattern");
    {
      std::string longval = "low=" + std::string(200, '1') + "\n";
      ok(!rc_deserialize(longval.c_str(), &b, NULL), "reject over-long value");
    }
    /* a truncated config file (power loss mid-write) must not be accepted
       as a partially-valid config */
    char full[RC_SERIAL_MAX];
    RingConfig a; rc_defaults(&a); a.low_mg_dl = 70; a.high_mg_dl = 200;
    rc_serialize(&a, full, sizeof(full));
    std::string trunc(full, strlen(full) / 2);
    RingConfig t;
    bool accepted = rc_deserialize(trunc.c_str(), &t, NULL);
    ok(!accepted || rc_validate(&t, NULL), "truncated file rejected or still valid");
  }

  section("config: pattern grammar");
  {
    RcPattern p;
    ok(rc_pattern_parse("400/200,400/0", &p) && p.count == 2, "two pulses");
    ok(rc_pattern_parse("100/0", &p) && p.count == 1, "one pulse");
    ok(!rc_pattern_parse("", &p), "reject empty");
    ok(!rc_pattern_parse("400", &p), "reject missing slash");
    ok(!rc_pattern_parse("400/", &p), "reject missing off");
    ok(!rc_pattern_parse("/200", &p), "reject missing on");
    ok(!rc_pattern_parse("400/200,", &p), "reject trailing comma");
    ok(!rc_pattern_parse("400/200,,100/0", &p), "reject empty element");
    ok(!rc_pattern_parse("5/100", &p), "reject on below min");
    ok(!rc_pattern_parse("3000/100", &p), "reject on above max");
    ok(!rc_pattern_parse("400/9000", &p), "reject off above max");
    ok(!rc_pattern_parse("400/200,400/200,400/200,400/200,400/200,400/200,400/200", &p),
       "reject too many pulses");
    ok(!rc_pattern_parse("abc/def", &p), "reject non-numeric");

    ok(rc_pattern_parse("400/200,400/0", &p), "reparse for format test");
    char b[80];
    ok(rc_pattern_format(&p, b, sizeof(b)) > 0 && strcmp(b, "400/200,400/0") == 0,
       std::string("format round trip: ") + b);
    char tiny[4];
    ok(rc_pattern_format(&p, tiny, sizeof(tiny)) == -1, "format rejects short buffer");
  }
}

/* ================= command protocol ================= */

struct MockHost {
  uint16_t mv = 4012;
  uint8_t  pct = 87;
  int      chg = 0;
  int      have = 1;
  int32_t  sgv = 112;
  const char *dir = "Flat";
  uint32_t age = 42;
  int      stale = 0;
  const char *alert = "none";
  uint32_t up = 3600;
  std::string last_buzz;
  int      buzz_calls = 0;
  int      saves = 0;
  int      dirty = 0;
  int      resets = 0;
  int      save_ok = 1;
};

static CmdHost make_host(MockHost *m)
{
  CmdHost h;
  memset(&h, 0, sizeof(h));
  h.ctx = m;
  h.batt_mv       = [](void *c){ return ((MockHost*)c)->mv; };
  h.batt_pct      = [](void *c){ return ((MockHost*)c)->pct; };
  h.charging      = [](void *c){ return ((MockHost*)c)->chg; };
  h.have_reading  = [](void *c){ return ((MockHost*)c)->have; };
  h.sgv           = [](void *c){ return ((MockHost*)c)->sgv; };
  h.direction     = [](void *c){ return ((MockHost*)c)->dir; };
  h.reading_age_s = [](void *c){ return ((MockHost*)c)->age; };
  h.is_stale      = [](void *c){ return ((MockHost*)c)->stale; };
  h.alert_name    = [](void *c){ return ((MockHost*)c)->alert; };
  h.uptime_s      = [](void *c){ return ((MockHost*)c)->up; };
  h.buzz          = [](void *c, const char *n){
      MockHost *m = (MockHost*)c;
      if (strcmp(n,"low") && strcmp(n,"high") && strcmp(n,"stale") &&
          strcmp(n,"tick") && strcmp(n,"battery") && strcmp(n,"fault") &&
          strcmp(n,"giveup")) return 0;
      m->last_buzz = n; m->buzz_calls++; return 1; };
  h.save          = [](void *c){ MockHost *m=(MockHost*)c; m->saves++; return m->save_ok; };
  h.mark_dirty    = [](void *c){ ((MockHost*)c)->dirty++; };
  h.factory_reset = [](void *c){ ((MockHost*)c)->resets++; return 1; };
  return h;
}

static std::string run(const char *line, RingConfig *cfg, const CmdHost *h)
{
  char resp[CMD_RESP_MAX];
  int n = cmd_execute(line, cfg, h, resp, sizeof(resp));
  ok(n == (int)strlen(resp), std::string("returned length matches strlen for ") + show(line));
  return std::string(resp);
}

static void test_commands()
{
  section("command: routing");
  ok(cmd_is_command("!VER"), "'!' is a command");
  ok(!cmd_is_command("112,Flat,1789238400000"), "reading is not a command");
  ok(!cmd_is_command(""), "empty is not a command");

  MockHost m;
  CmdHost h = make_host(&m);
  RingConfig cfg; rc_defaults(&cfg);

  section("command: query");
  ok(run("!VER", &cfg, &h).rfind("#VER glucose-ring " FW_VERSION, 0) == 0, "VER");
  ok(run("!GET THRESH", &cfg, &h) == "#THRESH low=unset high=unset", "THRESH unset");
  ok(run("!GET STALE", &cfg, &h) == "#STALE 15", "STALE");
  ok(run("!GET INTENSITY", &cfg, &h) == "#INTENSITY 100", "INTENSITY");
  ok(run("!GET STALEREPEATS", &cfg, &h) == "#STALEREPEATS 6", "STALEREPEATS");
  ok(run("!GET PATTERN low", &cfg, &h) == "#PATTERN low 400/200,400/200,400/0", "PATTERN low");
  ok(run("!GET BATT", &cfg, &h) == "#BATT mv=4012 pct=87 chg=0", "BATT");
  ok(run("!GET STATUS", &cfg, &h) ==
     "#STATUS sgv=112 dir=Flat age_s=42 stale=0 alert=none up_s=3600", "STATUS");

  m.have = 0;
  ok(run("!GET STATUS", &cfg, &h).rfind("#STATUS sgv=none", 0) == 0, "STATUS with no reading");
  m.have = 1;

  section("command: set");
  ok(run("!SET THRESH 70 200", &cfg, &h) == "#OK SET THRESH", "SET THRESH");
  ok(cfg.low_mg_dl == 70 && cfg.high_mg_dl == 200, "thresholds applied");
  ok(m.dirty > 0, "mark_dirty called");
  ok(run("!GET THRESH", &cfg, &h) == "#THRESH low=70 high=200", "THRESH reflects set");

  ok(run("!SET THRESH unset unset", &cfg, &h) == "#OK SET THRESH", "SET THRESH unset");
  ok(cfg.low_mg_dl == RC_UNSET, "unset accepted");
  run("!SET THRESH 70 200", &cfg, &h);

  /* a rejected SET must leave the live config untouched */
  {
    RingConfig before = cfg;
    std::string r = run("!SET THRESH 200 70", &cfg, &h);
    ok(r.rfind("#ERR SET", 0) == 0, std::string("inverted rejected: ") + r);
    ok(memcmp(&before, &cfg, sizeof(cfg)) == 0, "config unchanged after rejected SET");

    r = run("!SET THRESH 10 200", &cfg, &h);
    ok(r.rfind("#ERR SET", 0) == 0, "low out of range rejected");
    ok(memcmp(&before, &cfg, sizeof(cfg)) == 0, "config unchanged after range failure");
  }

  ok(run("!SET INTENSITY 50", &cfg, &h) == "#OK SET INTENSITY", "SET INTENSITY");
  ok(cfg.intensity == 50, "intensity applied");
  ok(run("!SET INTENSITY 200", &cfg, &h).rfind("#ERR SET", 0) == 0, "intensity 200 rejected");
  ok(run("!SET INTENSITY 0", &cfg, &h).rfind("#ERR SET", 0) == 0, "intensity 0 rejected");
  ok(run("!SET STALE 0", &cfg, &h).rfind("#ERR SET", 0) == 0, "stale 0 rejected");
  ok(run("!SET STALE 30", &cfg, &h) == "#OK SET STALE", "stale 30 ok");
  ok(run("!SET STALEREPEATS 3", &cfg, &h) == "#OK SET STALEREPEATS", "SET STALEREPEATS");
  ok(cfg.stale_max_repeats == 3, "stale_max_repeats applied");
  ok(run("!SET STALEREPEATS 0", &cfg, &h) == "#OK SET STALEREPEATS", "SET STALEREPEATS 0 allowed");
  ok(run("!SET STALEREPEATS 256", &cfg, &h).rfind("#ERR SET", 0) == 0, "256 rejected");
  ok(run("!SET STALEREPEATS -1", &cfg, &h).rfind("#ERR SET", 0) == 0, "negative rejected");
  run("!SET STALEREPEATS 6", &cfg, &h);
  ok(run("!SET PATTERN low 500/300", &cfg, &h) == "#OK SET PATTERN", "SET PATTERN");
  ok(cfg.pat_low.count == 1 && cfg.pat_low.pulses[0].on_ms == 500, "pattern applied");
  ok(run("!SET PATTERN bogus 500/300", &cfg, &h).rfind("#ERR SET", 0) == 0, "unknown pattern");
  ok(run("!SET PATTERN low 9/9", &cfg, &h).rfind("#ERR SET", 0) == 0, "bad pattern rejected");

  section("command: actions");
  int before_buzz = m.buzz_calls;
  ok(run("!BUZZ low", &cfg, &h) == "#OK BUZZ low", "BUZZ low");
  ok(m.buzz_calls == before_buzz + 1 && m.last_buzz == "low", "buzz dispatched");
  ok(run("!BUZZ nope", &cfg, &h).rfind("#ERR BUZZ", 0) == 0, "unknown buzz pattern");
  ok(run("!BUZZ fault", &cfg, &h) == "#OK BUZZ fault", "BUZZ fault");
  ok(run("!BUZZ giveup", &cfg, &h) == "#OK BUZZ giveup", "BUZZ giveup");
  ok(run("!SAVE", &cfg, &h) == "#OK SAVE", "SAVE");
  ok(m.saves == 1, "save called");
  m.save_ok = 0;
  ok(run("!SAVE", &cfg, &h).rfind("#ERR SAVE", 0) == 0, "SAVE failure surfaced");
  m.save_ok = 1;

  ok(run("!RESET CONFIG", &cfg, &h) == "#OK RESET CONFIG", "RESET CONFIG");
  ok(cfg.low_mg_dl == RC_UNSET && cfg.stale_minutes == 15, "config back to defaults");
  ok(cfg.stale_max_repeats == 6, "RESET restores stale_max_repeats");
  ok(m.resets == 1, "factory_reset called");
  ok(run("!RESET", &cfg, &h).rfind("#ERR RESET", 0) == 0, "RESET needs CONFIG");

  section("command: errors and hostile input");
  ok(run("!NOPE", &cfg, &h).rfind("#ERR ? unknown verb", 0) == 0, "unknown verb");
  ok(run("!", &cfg, &h).rfind("#ERR ? empty", 0) == 0, "bare prefix");
  ok(run("!GET", &cfg, &h).rfind("#ERR GET missing key", 0) == 0, "GET without key");
  ok(run("!GET NOPE", &cfg, &h).rfind("#ERR GET unknown key", 0) == 0, "GET unknown key");
  ok(run("!SET", &cfg, &h).rfind("#ERR SET missing key", 0) == 0, "SET without key");
  ok(run("!SET THRESH 70", &cfg, &h).rfind("#ERR SET", 0) == 0, "SET THRESH one arg");
  ok(run("!GET   THRESH", &cfg, &h) == "#THRESH low=unset high=unset", "extra spaces ok");
  ok(run("  !VER", &cfg, &h).rfind("#ERR", 0) == 0, "leading space is not a command");

  {  /* over-long token must be refused, not truncated into a valid verb */
    std::string big = "!SET THRESH " + std::string(100, '9') + " 200";
    ok(run(big.c_str(), &cfg, &h).rfind("#ERR", 0) == 0, "over-long token rejected");
    std::string many = "!GET";
    for (int i = 0; i < 20; i++) many += " x";
    ok(run(many.c_str(), &cfg, &h).rfind("#ERR", 0) == 0, "too many tokens rejected");
  }

  section("command: response buffer safety");
  for (size_t sz = 1; sz <= 24; sz++) {
    char small[32];
    memset(small, 0x7F, sizeof(small));
    int n = cmd_execute("!GET STATUS", &cfg, &h, small, sz);
    ok(strlen(small) < sz, "NUL-terminated within " + std::to_string(sz));
    ok(n >= 0 && (size_t)n < sz, "length in bounds for size " + std::to_string(sz));
    ok((unsigned char)small[sz] == 0x7F, "no write past size " + std::to_string(sz));
  }
  {
    char resp[CMD_RESP_MAX];
    ok(cmd_execute(NULL, &cfg, &h, resp, sizeof(resp)) > 0, "null line handled");
    ok(cmd_execute("!VER", NULL, &h, resp, sizeof(resp)) > 0, "null cfg handled");
    ok(cmd_execute("!VER", &cfg, NULL, resp, sizeof(resp)) > 0, "null host handled");
    ok(cmd_execute("!GET BATT", &cfg, NULL, resp, sizeof(resp)) > 0 &&
       std::string(resp).rfind("#ERR", 0) == 0, "BATT without host -> error");
    ok(cmd_execute("!VER", &cfg, &h, resp, 0) == 0, "zero-size buffer safe");
  }
  {  /* commands must not mutate the caller's line */
    char line[] = "!SET THRESH 70 200";
    char copy[sizeof(line)];
    memcpy(copy, line, sizeof(line));
    char resp[CMD_RESP_MAX];
    cmd_execute(line, &cfg, &h, resp, sizeof(resp));
    ok(memcmp(line, copy, sizeof(line)) == 0, "command line not mutated");
  }
}

int main(void)
{
  printf("glucose-ring host tests (parser / config / command protocol)\n");
  test_parser_valid();
  test_parser_malformed();
  test_parser_truncated();
  test_parser_range();
  test_parser_garbage();
  test_parser_contract();
  test_config();
  test_commands();

  printf("\n=====================================\n");
  printf("passed: %d   failed: %d\n", g_pass, g_fail);
  printf("=====================================\n");
  return g_fail == 0 ? 0 : 1;
}
