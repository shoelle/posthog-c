#include "ph_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <errno.h>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#endif

/* --- Clocks ----------------------------------------------------------- */

uint64_t ph_now_mono_ns(void) {
#if defined(_WIN32)
    /* GetTickCount64 includes time spent suspended. Millisecond resolution is
     * sufficient for wire timestamps and avoids racy lazy clock state. */
    return (uint64_t)GetTickCount64() * 1000000ull;
#elif defined(__APPLE__)
    mach_timebase_info_data_t info;
    uint64_t ticks = mach_continuous_time(); /* unlike mach_absolute_time, includes sleep */
    uint64_t whole, rem;
    mach_timebase_info(&info);
    whole = ticks / info.denom;
    rem = ticks % info.denom;
    return whole * info.numer + (rem * info.numer) / info.denom;
#else
    struct timespec ts;
#if defined(CLOCK_BOOTTIME)
    clock_gettime(CLOCK_BOOTTIME, &ts); /* Linux: includes system suspend */
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t ph_now_wall_ns(void) {
#if defined(_WIN32)
    /* FILETIME is 100ns ticks since 1601-01-01; shift to the Unix epoch. */
    FILETIME ft;
    ULARGE_INTEGER u;
    const uint64_t EPOCH_DIFF_100NS = 116444736000000000ull;
    GetSystemTimePreciseAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - EPOCH_DIFF_100NS) * 100ull;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t ph_seed_u64(void) {
    uint64_t x = 0;
#if defined(_WIN32)
    if (BCryptGenRandom(NULL, (PUCHAR)&x, (ULONG)sizeof(x),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return x;
#elif defined(__APPLE__)
    arc4random_buf(&x, sizeof(x));
    return x;
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
    {
        unsigned char *p = (unsigned char *)&x;
        size_t have = 0;
        while (have < sizeof(x)) {
            ssize_t n = getrandom(p + have, sizeof(x) - have, 0);
            if (n > 0) have += (size_t)n;
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
        if (have == sizeof(x)) return x;
        /* Old kernels/seccomp may reject getrandom. Fall back to the OS random
         * device before using the non-cryptographic emergency mixer below. */
        {
            int fd = open("/dev/urandom", O_RDONLY);
            if (fd >= 0) {
                have = 0;
                while (have < sizeof(x)) {
                    ssize_t n = read(fd, p + have, sizeof(x) - have);
                    if (n > 0) have += (size_t)n;
                    else if (n < 0 && errno == EINTR) continue;
                    else break;
                }
                close(fd);
                if (have == sizeof(x)) return x;
            }
        }
    }
#endif
    /* Emergency fallback for restricted/older environments (including WASM,
     * where this native UUID salt is unused). */
    x = ph_now_wall_ns();
    x ^= ph_now_mono_ns() * 0x9E3779B97F4A7C15ull;
    x ^= (uint64_t)(uintptr_t)&x << 17;
#if defined(_WIN32)
    x ^= (uint64_t)GetCurrentProcessId() * 0xD1B54A32D192ED03ull;
#endif
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

uint64_t ph_correct_wall_epoch(uint64_t epoch_wall_ns, uint64_t epoch_mono_ns,
                               uint64_t now_wall_ns, uint64_t now_mono_ns,
                               uint64_t threshold_ns) {
    uint64_t delta, predicted, skew;
    if (now_mono_ns < epoch_mono_ns) return epoch_wall_ns;
    delta = now_mono_ns - epoch_mono_ns;
    predicted = UINT64_MAX - epoch_wall_ns < delta
                    ? UINT64_MAX : epoch_wall_ns + delta;
    skew = now_wall_ns >= predicted ? now_wall_ns - predicted
                                    : predicted - now_wall_ns;
    if (skew <= threshold_ns) return epoch_wall_ns;
    if (now_wall_ns >= predicted)
        return UINT64_MAX - epoch_wall_ns < skew
                   ? UINT64_MAX : epoch_wall_ns + skew;
    return skew > epoch_wall_ns ? 0 : epoch_wall_ns - skew;
}

/* --- ISO-8601 --------------------------------------------------------- */

void ph_format_iso8601(uint64_t wall_ns, char *out, size_t cap) {
    time_t secs = (time_t)(wall_ns / 1000000000ull);
    unsigned ms = (unsigned)((wall_ns / 1000000ull) % 1000ull);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &secs);
#else
    gmtime_r(&secs, &tmv);
#endif
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
}

/* --- UUIDv7 ----------------------------------------------------------- */

static uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

void ph_uuid_v7(uint64_t wall_ms, uint64_t salt, uint64_t seq, char out[37]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t u[16];
    uint64_t r0 = splitmix64(salt ^ (seq * 0x9E3779B97F4A7C15ull));
    uint64_t r1 = splitmix64(r0 ^ 0xD1B54A32D192ED03ull);
    int i, j;

    /* 48-bit big-endian millisecond timestamp. */
    u[0] = (uint8_t)(wall_ms >> 40);
    u[1] = (uint8_t)(wall_ms >> 32);
    u[2] = (uint8_t)(wall_ms >> 24);
    u[3] = (uint8_t)(wall_ms >> 16);
    u[4] = (uint8_t)(wall_ms >> 8);
    u[5] = (uint8_t)(wall_ms);

    /* 74 random bits from (r0, r1) with the version + variant nibbles set. */
    u[6] = 0x70 | (uint8_t)((r0 >> 8) & 0x0F);  /* version 7 */
    u[7] = (uint8_t)(r0 & 0xFF);
    u[8] = 0x80 | (uint8_t)((r0 >> 16) & 0x3F); /* variant 10 */
    u[9] = (uint8_t)((r0 >> 24) & 0xFF);
    u[10] = (uint8_t)(r1 >> 0);
    u[11] = (uint8_t)(r1 >> 8);
    u[12] = (uint8_t)(r1 >> 16);
    u[13] = (uint8_t)(r1 >> 24);
    u[14] = (uint8_t)(r1 >> 32);
    u[15] = (uint8_t)(r1 >> 40);

    /* 8-4-4-4-12 with dashes. */
    j = 0;
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[j++] = '-';
        out[j++] = hex[(u[i] >> 4) & 0xF];
        out[j++] = hex[u[i] & 0xF];
    }
    out[j] = '\0';
}
