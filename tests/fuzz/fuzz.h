/*
 * fuzz.h - the tiny contract every posthog-c fuzz target implements.
 *
 * Targets use the standard libFuzzer entry point so a real coverage-guided
 * libFuzzer/AFL build (Linux CI) can drive them unchanged. On toolchains
 * without libFuzzer (e.g. zig cc on Windows), fuzz_run.c provides a portable
 * main() that mutates each target's seeds under whatever sanitizer is available.
 */
#ifndef PH_FUZZ_H
#define PH_FUZZ_H

#include <stddef.h>
#include <stdint.h>

/* The one function a fuzz target must define (libFuzzer-compatible). */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Seed inputs the standalone driver mutates. `len` carries the length so seeds
 * may contain NUL bytes (HTTP bodies do). A target returns NULL/0 for none. */
typedef struct {
    const char *data;
    size_t len;
} fuzz_seed;

const fuzz_seed *fuzz_seeds(size_t *count);

#endif /* PH_FUZZ_H */
