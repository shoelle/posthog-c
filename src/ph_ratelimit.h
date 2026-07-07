/*
 * ph_ratelimit.h — server-directed send backpressure.
 *
 * When the ingestion endpoint pushes back — HTTP 429 Too Many Requests
 * (RFC 6585), or a 503 carrying a Retry-After (RFC 9110 s10.2.3) — the sender
 * should stop sending until the server's window elapses, rather than draining
 * batch after batch into a throttle. This is a tiny, pure state machine over a
 * single monotonic deadline, plus a Retry-After parser. It touches no globals
 * and no I/O, so it is unit-tested directly (parser vectors + the deadline
 * logic) with no socket in the loop.
 *
 * The deadline is a monotonic tick, not wall time, so a system clock change
 * cannot shorten or extend a backoff — the same reason the capture path times
 * off ph_now_mono_ns (see ph_time.h).
 */
#ifndef PH_RATELIMIT_H
#define PH_RATELIMIT_H

#include <stdint.h>

/* Default hold when a 429 arrives with no (or an unparseable) Retry-After.
 * There is no spec-defined default; 60s is the conventional client fallback.
 * One batch per minute probes the limit without hammering it. Override with -D. */
#ifndef PH_RL_DEFAULT_BACKOFF_MS
#define PH_RL_DEFAULT_BACKOFF_MS 60000
#endif

/* Ceiling on any single backoff. Bounds an absurd or hostile Retry-After and
 * guards the ns arithmetic from overflow. 24h matches a generous upper limit. */
#ifndef PH_RL_MAX_BACKOFF_MS
#define PH_RL_MAX_BACKOFF_MS (24 * 60 * 60 * 1000)
#endif

typedef struct ph_ratelimit {
    uint64_t disabled_until_mono_ns; /* 0 = not limited */
} ph_ratelimit;

/* Clear to the not-limited state. */
void ph_ratelimit_init(ph_ratelimit *rl);

/*
 * Parse an RFC 9110 Retry-After header *value*. Two forms are accepted:
 *   delay-seconds  a non-negative integer count of seconds ("120")
 *   HTTP-date      an IMF-fixdate ("Fri, 31 Dec 1999 23:59:59 GMT")
 * Returns the delay in milliseconds (>= 0), or -1 if the value is NULL, empty,
 * or unrecognized. For an HTTP-date, `now_wall_ns` converts the absolute time
 * to a relative delay; a date already in the past yields 0. The result is
 * clamped to PH_RL_MAX_BACKOFF_MS.
 */
long ph_ratelimit_parse_retry_after(const char *value, uint64_t now_wall_ns);

/*
 * Record an HTTP response. A 429 always engages the limiter; a 503 engages it
 * only when it carries a usable Retry-After (otherwise ordinary 5xx retry/
 * backoff handles it). The window honors Retry-After when present, else
 * PH_RL_DEFAULT_BACKOFF_MS, plus a small positive jitter so many clients that
 * were throttled together do not all resume in lockstep (spec permits waiting
 * longer than Retry-After). `retry_after` may be NULL. Returns 1 if the limiter
 * is now engaged, else the current blocked state.
 */
int ph_ratelimit_note_response(ph_ratelimit *rl, int status,
                               const char *retry_after, uint64_t now_mono_ns,
                               uint64_t now_wall_ns);

/* 1 if sends should be held right now. */
int ph_ratelimit_blocked(const ph_ratelimit *rl, uint64_t now_mono_ns);

/* Milliseconds until the window clears (0 if not blocked) — for diagnostics. */
uint64_t ph_ratelimit_remaining_ms(const ph_ratelimit *rl, uint64_t now_mono_ns);

#endif /* PH_RATELIMIT_H */
