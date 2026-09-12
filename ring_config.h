/*
 * ring_config.h -- persisted configuration: struct, defaults, validation,
 * and text (de)serialisation.
 *
 * PURE: no Arduino, no filesystem. The .ino does the file I/O; this module
 * owns the format and the rules, so both are host-testable.
 */
#ifndef RING_CONFIG_H_
#define RING_CONFIG_H_

#include <stdint.h>
#include <stddef.h>

#define FW_VERSION      "3.0.0"
#define PROTO_VERSION   3   /* 3: ring pokes (#REQ), !TIME, readings judged by trusted time */

#define RC_SCHEMA_VERSION      2   /* 2 added stale_max_repeats */
#define RC_UNSET               (-1)
#define RC_PATTERN_MAX_PULSES  6

/* Serialised config must fit one flash write comfortably. */
#define RC_SERIAL_MAX          512

/* ---- validation bounds (also the bounds !SET commands enforce) ---- */
#define RC_SGV_MIN            20
#define RC_SGV_MAX           600
#define RC_STALE_MIN_MIN       1
#define RC_STALE_MIN_MAX    1440
#define RC_REPEAT_MIN_MIN      1
#define RC_REPEAT_MIN_MAX   1440
#define RC_INTENSITY_MIN       1     /* 0 would mean "never buzz" -- use a pattern for that */
#define RC_INTENSITY_MAX     100
#define RC_STALE_REPEATS_MAX 255     /* 0 is legal and means "never stop" */
#define RC_PULSE_ON_MIN       10
#define RC_PULSE_ON_MAX     2000
#define RC_PULSE_OFF_MAX    5000

typedef struct { uint16_t on_ms; uint16_t off_ms; } RcPulse;

typedef struct {
  uint8_t count;
  RcPulse pulses[RC_PATTERN_MAX_PULSES];
} RcPattern;

typedef struct {
  uint16_t  version;
  int32_t   low_mg_dl;      /* RC_UNSET => that side never alerts */
  int32_t   high_mg_dl;     /* RC_UNSET => that side never alerts */
  uint16_t  stale_minutes;
  uint16_t  repeat_minutes;
  uint8_t   intensity;      /* percent of full motor drive, 1..100 */
  /*
   * How many times a continuous stale condition may re-buzz before the ring
   * gives up, signals that it is giving up, and goes quiet until fresh data
   * arrives. 0 means never stop.
   *
   * A cap exists because a ring left in a drawer buzzing every few minutes
   * for hours trains the wearer to ignore it -- and an ignored alert is the
   * real hazard, worse than the silence the repeats were guarding against.
   */
  uint8_t   stale_max_repeats;
  RcPattern pat_low;
  RcPattern pat_high;
  RcPattern pat_stale;
} RingConfig;

/*
 * The MISS pattern ("the phone stopped answering") is fixed, not
 * configurable: it must never feel like a high or a low. rc_validate()
 * therefore refuses a low/high/stale pattern identical to it. That is an
 * exact-match guard only; a near copy is still possible.
 */
extern const RcPattern RC_PAT_MISS;

/*
 * Factory defaults.
 *
 * Thresholds default to RC_UNSET *on purpose*. Picking clinical numbers is
 * not this firmware's job; an unset side simply never alerts. Set them with
 * "!SET THRESH" or by editing DEFAULT_LOW_MG_DL / DEFAULT_HIGH_MG_DL in the
 * sketch before flashing.
 */
void rc_defaults(RingConfig *c);

/* why: optional; set to a static reason string on failure. */
bool rc_validate(const RingConfig *c, const char **why);

/* Writes NUL-terminated key=value text. Returns length, or -1 if it
 * would not fit. */
int  rc_serialize(const RingConfig *c, char *buf, size_t bufsize);

/*
 * Parse key=value text. Unknown keys are IGNORED (forward compatibility with
 * configs written by a newer firmware); missing keys keep their default. The
 * result is validated before being returned, so a corrupt file cannot install
 * an out-of-range config.
 */
bool rc_deserialize(const char *text, RingConfig *out, const char **why);

/* "low" | "high" | "stale" -> pattern, or NULL. Non-const and const forms. */
RcPattern       *rc_pattern_by_name(RingConfig *c, const char *name);
const RcPattern *rc_pattern_by_name_const(const RingConfig *c, const char *name);

/* "400/200,400/0" <-> RcPattern */
bool rc_pattern_parse(const char *s, RcPattern *out);
int  rc_pattern_format(const RcPattern *p, char *buf, size_t bufsize);

#endif /* RING_CONFIG_H_ */
