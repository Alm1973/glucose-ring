/*
 * reading_parser.h -- glucose reading wire-format parser.
 *
 * PURE: no Arduino, no hardware, no globals. stdint/stddef only.
 * Compiled both into the firmware and into tests/test_all.cpp on the host,
 * so the tests exercise the exact code that runs on the ring.
 */
#ifndef READING_PARSER_H_
#define READING_PARSER_H_

#include <stdint.h>
#include <stddef.h>

/* Longest Nightscout direction is "RATE OUT OF RANGE" (17 chars). 24 leaves
 * headroom; over-length is an ERROR, never a silent truncation. */
#define RP_DIRECTION_MAX 24

typedef enum {
  RP_OK = 0,
  RP_ERR_NULL,           /* null pointer argument                       */
  RP_ERR_EMPTY,          /* empty line                                  */
  RP_ERR_FIELD_COUNT,    /* not exactly three comma-separated fields    */
  RP_ERR_SGV_FORMAT,     /* field 1 not bare decimal digits             */
  RP_ERR_SGV_RANGE,      /* field 1 outside limits                      */
  RP_ERR_DIRECTION,      /* field 2 too long or has non-printable bytes */
  RP_ERR_EPOCH_FORMAT,   /* field 3 not bare decimal digits             */
  RP_ERR_EPOCH_RANGE     /* field 3 below the sanity floor, or overflow */
} RpResult;

typedef struct {
  int32_t  sgv;
  char     direction[RP_DIRECTION_MAX];
  uint64_t epoch_ms;
} RpReading;

typedef struct {
  int32_t  sgv_min;
  int32_t  sgv_max;
  uint64_t epoch_ms_min;
} RpLimits;

extern const RpLimits RP_DEFAULT_LIMITS;

/*
 * Parse "<sgv>,<direction>,<epoch_ms>".
 *
 * `line` is NUL-terminated and is NOT modified (the previous in-place
 * strchr/NUL-poking version could not be handed a string literal).
 * `out` is only written on RP_OK.
 *
 * Strictness is deliberate -- see PROTOCOL.md:
 *   - digits only, no sign, no whitespace, no "+", no "1.0e2"
 *   - direction may be empty and may contain spaces ("NOT COMPUTABLE")
 *   - a fourth field is rejected rather than ignored
 */
RpResult rp_parse(const char *line, const RpLimits *limits, RpReading *out);

/* Stable short name for a result code, for logging and tests. */
const char *rp_result_name(RpResult r);

#endif /* READING_PARSER_H_ */
