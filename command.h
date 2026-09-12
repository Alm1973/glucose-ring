/*
 * command.h -- text command protocol carried over the same NUS link.
 *
 * PURE: no Arduino. Everything the commands need from the hardware arrives
 * through CmdHost function pointers, so the whole protocol is host-testable
 * with a mock. See PROTOCOL.md for the wire format.
 */
#ifndef COMMAND_H_
#define COMMAND_H_

#include "ring_config.h"

#define CMD_PREFIX      '!'
#define CMD_RESP_MAX    200
#define CMD_TIME_MIN_MS 1600000000000ULL   /* same floor as the reading parser */

/* Hardware/state the command layer may need. Any pointer may be NULL; the
 * command that needs it then answers "#ERR ... unsupported". */
typedef struct {
  void *ctx;

  uint16_t    (*batt_mv)      (void *ctx);
  uint8_t     (*batt_pct)     (void *ctx);
  int         (*charging)     (void *ctx);   /* 1 charging, 0 not, -1 unknown */

  int         (*have_reading) (void *ctx);
  int32_t     (*sgv)          (void *ctx);
  const char *(*direction)    (void *ctx);
  uint32_t    (*reading_age_s)(void *ctx);
  int         (*is_stale)     (void *ctx);
  const char *(*alert_name)   (void *ctx);
  uint32_t    (*uptime_s)     (void *ctx);

  int         (*buzz)         (void *ctx, const char *pattern_name);
  int         (*save)         (void *ctx);   /* persist now; 1 ok, 0 fail */
  void        (*mark_dirty)   (void *ctx);   /* config changed -> autosave later */
  int         (*factory_reset)(void *ctx);

  /* protocol 3: trusted clock and the poke cycle (liveness.h) */
  int         (*set_time)     (void *ctx, uint64_t epoch_ms);   /* !TIME; 1 ok */
  int         (*link_misses)  (void *ctx);
  int         (*clock_synced) (void *ctx);
  uint32_t    (*next_poke_s)  (void *ctx);
} CmdHost;

/* True if the line should be routed to cmd_execute rather than the
 * glucose-reading parser. */
int cmd_is_command(const char *line);

/*
 * Execute one command line (leading '!' included, no trailing newline).
 * Always writes a NUL-terminated response into `resp` and returns its
 * length. Never writes more than resp_size-1 bytes. Never mutates `line`.
 * `cfg` is updated in place for !SET commands.
 */
int cmd_execute(const char *line, RingConfig *cfg, const CmdHost *host,
                char *resp, size_t resp_size);

#endif /* COMMAND_H_ */
