#include "ph_time.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* --- Clocks ----------------------------------------------------------- */

uint64_t ph_now_mono_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq; /* fixed for the life of the process */
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    /* Scale to ns without overflow: (whole seconds) + (remainder ticks). */
    {
        uint64_t q = (uint64_t)freq.QuadPart;
        uint64_t t = (uint64_t)now.QuadPart;
        uint64_t secs = t / q;
        uint64_t rem = t % q;
        return secs * 1000000000ull + (rem * 1000000000ull) / q;
    }
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
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
    /* Mix a few independent sources; splitmix64-finalize so the low bits are
     * well distributed. Good enough to salt UUIDs, not for security. */
    uint64_t x = ph_now_wall_ns();
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
