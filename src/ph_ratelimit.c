#include "ph_ratelimit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ph_ratelimit_init(ph_ratelimit *rl) { rl->disabled_until_mono_ns = 0; }

void ph_ratelimit_arm(ph_ratelimit *rl, uint64_t window_ms, uint64_t now_mono_ns) {
    /* Positive jitter of up to ~1/8 of the window, derived from the low bits of
     * the monotonic clock (distinct across processes) so a fleet throttled at
     * once does not resume in a single thundering herd. No RNG needed. */
    uint64_t jitter = now_mono_ns % (window_ms / 8 + 1);
    window_ms += jitter;
    if (window_ms > (uint64_t)PH_RL_MAX_BACKOFF_MS)
        window_ms = (uint64_t)PH_RL_MAX_BACKOFF_MS;
    rl->disabled_until_mono_ns = now_mono_ns + window_ms * 1000000ull;
}

/* Days from the civil date to 1970-01-01 (Howard Hinnant's algorithm). Valid
 * for the proleptic Gregorian calendar; y is the full year, m in [1,12]. */
static long days_from_civil(long y, unsigned m, unsigned d) {
    long era;
    unsigned yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);            /* [0, 399] */
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* [0, 365] */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          /* [0, 146096] */
    return era * 146097 + (long)doe - 719468;
}

/* Three-letter English month name -> [1,12], or 0 if unrecognized. */
static int month_num(const char *mon) {
    static const char names[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int i;
    for (i = 0; i < 12; i++)
        if (strncmp(mon, names[i], 3) == 0) return i + 1;
    return 0;
}

long ph_ratelimit_parse_retry_after(const char *value, uint64_t now_wall_ns) {
    const char *p;
    const char *rest;
    unsigned d, Y, hh, mm, ss;
    char mon[4];
    int is_digits;

    if (!value) return -1;
    while (*value == ' ' || *value == '\t') value++;
    if (!*value) return -1;

    /* delay-seconds: the token is entirely ASCII digits. */
    is_digits = 1;
    for (p = value; *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n'; p++) {
        if (*p < '0' || *p > '9') {
            is_digits = 0;
            break;
        }
    }
    if (is_digits && p != value) {
        /* Only delay-seconds if nothing but trailing whitespace follows the
         * digits; otherwise this is the leading day of an HTTP-date. */
        const char *q = p;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == '\0') {
            /* Cap seconds before the *1000 so the multiply cannot overflow. */
            long secs = strtol(value, NULL, 10);
            if (secs < 0 || secs > (long)PH_RL_MAX_BACKOFF_MS / 1000)
                return (long)PH_RL_MAX_BACKOFF_MS;
            return secs * 1000L;
        }
    }

    /* IMF-fixdate: "Day, DD Mon YYYY HH:MM:SS GMT". Skip the leading day-name
     * (up to and including its comma) so we tolerate any/none, then scan the
     * fixed remainder. Obsolete RFC 850 / asctime forms are not parsed. */
    rest = strchr(value, ',');
    rest = rest ? rest + 1 : value;
    while (*rest == ' ') rest++;
    mon[0] = '\0';
    if (sscanf(rest, "%u %3s %u %u:%u:%u", &d, mon, &Y, &hh, &mm, &ss) == 6) {
        int M = month_num(mon);
        long long epoch, now_s, delta;
        if (M == 0 || hh > 23 || mm > 59 || ss > 60) return -1; /* 60 = leap sec */
        epoch = (long long)days_from_civil((long)Y, (unsigned)M, d) * 86400
            + (long long)hh * 3600 + (long long)mm * 60 + ss;
        now_s = (long long)(now_wall_ns / 1000000000ull);
        delta = epoch - now_s;
        if (delta < 0) delta = 0;
        if (delta > (long long)PH_RL_MAX_BACKOFF_MS / 1000)
            return (long)PH_RL_MAX_BACKOFF_MS;
        return (long)(delta * 1000);
    }

    return -1;
}

int ph_ratelimit_note_response(ph_ratelimit *rl, int status,
                               const char *retry_after, uint64_t now_mono_ns,
                               uint64_t now_wall_ns) {
    long ra_ms;
    uint64_t window_ms;

    if (status != 429 && status != 503)
        return ph_ratelimit_blocked(rl, now_mono_ns);

    ra_ms = ph_ratelimit_parse_retry_after(retry_after, now_wall_ns);
    if (ra_ms >= 0) {
        window_ms = (uint64_t)ra_ms;
    } else if (status == 503) {
        /* 503 with no Retry-After: leave it to the caller's ordinary 5xx retry. */
        return ph_ratelimit_blocked(rl, now_mono_ns);
    } else {
        window_ms = PH_RL_DEFAULT_BACKOFF_MS;
    }

    ph_ratelimit_arm(rl, window_ms, now_mono_ns);
    return 1;
}

int ph_ratelimit_blocked(const ph_ratelimit *rl, uint64_t now_mono_ns) {
    return rl->disabled_until_mono_ns > now_mono_ns;
}

uint64_t ph_ratelimit_remaining_ms(const ph_ratelimit *rl, uint64_t now_mono_ns) {
    if (rl->disabled_until_mono_ns <= now_mono_ns) return 0;
    return (rl->disabled_until_mono_ns - now_mono_ns) / 1000000ull;
}
