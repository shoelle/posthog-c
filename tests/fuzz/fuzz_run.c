/*
 * fuzz_run.c — a portable standalone driver for the fuzz targets.
 *
 * Linked in only when libFuzzer isn't available (libFuzzer provides its own
 * main). No coverage feedback: it feeds every seed once, then hammers random
 * mutations of the seeds. Deterministic (fixed PRNG seed) so any crash
 * reproduces on a re-run; override iterations/seed via argv for triage:
 *     fuzz_jsonval [iterations] [rng_seed]
 * Run it under a sanitizer (ASan/UBSan) where the toolchain has one; without a
 * sanitizer it still catches faults (segfault / stack overflow) and hangs.
 */
#include "fuzz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_CAP (1u << 20) /* 1 MiB max mutated input */

static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}

/* Bytes that tend to matter for JSON + HTTP grammars. */
static const unsigned char interesting[] = {
    0x00, 0xFF, 0x7F, 0x80, '{', '}', '[', ']', '"', '\\', ':', ',',
    '\n', '\r', ' ', '\t', '0', '9', '-', '.', 'e', 't', 'f', 'n', 'u', 'x'
};

static size_t mutate(unsigned char *b, size_t len) {
    int ops = 1 + (int)(rnd() % 6), i;
    for (i = 0; i < ops; i++) {
        if (len == 0) {
            b[0] = (unsigned char)rnd();
            len = 1;
            continue;
        }
        switch (rnd() % 7) {
        case 0: b[rnd() % len] ^= (unsigned char)(1u << (rnd() % 8)); break;
        case 1: b[rnd() % len] = (unsigned char)rnd(); break;
        case 2: b[rnd() % len] = interesting[rnd() % sizeof interesting]; break;
        case 3: /* insert */
            if (len < FUZZ_CAP) {
                size_t p = rnd() % (len + 1);
                memmove(b + p + 1, b + p, len - p);
                b[p] = interesting[rnd() % sizeof interesting];
                len++;
            }
            break;
        case 4: { /* delete */
            size_t p = rnd() % len;
            memmove(b + p, b + p + 1, len - p - 1);
            len--;
            break;
        }
        case 5: { /* duplicate a block (grows structure — helps nesting) */
            size_t p = rnd() % len;
            size_t n = 1 + rnd() % (len - p);
            if (len + n <= FUZZ_CAP) {
                memmove(b + p + n, b + p, len - p);
                len += n;
            }
            break;
        }
        case 6: len = rnd() % (len + 1); break; /* truncate */
        }
    }
    return len;
}

int main(int argc, char **argv) {
    static unsigned char buf[FUZZ_CAP];
    size_t nseeds = 0, s;
    const fuzz_seed *seeds = fuzz_seeds(&nseeds);
    long iters = argc > 1 ? atol(argv[1]) : 200000;
    long it;

    if (argc > 2) g_rng = strtoull(argv[2], NULL, 0);
    if (!g_rng) g_rng = 1;

    /* Pass 1: every seed verbatim — catches structural bugs (deep nesting, etc.)
     * that random mutation is unlikely to synthesize. Flushed so a crash names
     * the culprit seed. */
    for (s = 0; s < nseeds; s++) {
        fprintf(stderr, "seed %zu/%zu (%zu bytes)\n", s + 1, nseeds, seeds[s].len);
        fflush(stderr);
        LLVMFuzzerTestOneInput((const uint8_t *)seeds[s].data, seeds[s].len);
    }

    /* Pass 2: mutate. */
    for (it = 0; it < iters; it++) {
        const fuzz_seed *sd = &seeds[rnd() % nseeds];
        size_t len = sd->len < FUZZ_CAP ? sd->len : FUZZ_CAP;
        int k, muts = 1 + (int)(rnd() % 8);
        memcpy(buf, sd->data, len);
        for (k = 0; k < muts; k++) len = mutate(buf, len);
        LLVMFuzzerTestOneInput(buf, len);
        if ((it % 20000) == 0) {
            fprintf(stderr, "\r%ld / %ld iterations", it, iters);
            fflush(stderr);
        }
    }
    fprintf(stderr, "\r%ld / %ld iterations\n", iters, iters);
    printf("OK: %ld seeds + %ld mutations, no crash\n", (long)nseeds, iters);
    return 0;
}
