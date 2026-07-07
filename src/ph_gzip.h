/*
 * ph_gzip.h — gzip a buffer for Content-Encoding: gzip on /batch/.
 *
 * PostHog's legacy /batch/ endpoint accepts gzip-compressed bodies (the server
 * SDKs do the same). We only ever compress, so this wraps the tiny vendored
 * `sdefl` DEFLATE compressor (third_party/sdefl) in a gzip container (RFC 1952):
 * 10-byte header + raw deflate + CRC-32 + input size. Native-only — the wasm
 * backend delegates to posthog-js, which compresses itself.
 */
#ifndef PH_GZIP_H
#define PH_GZIP_H

#include <stddef.h>

/* Compress `in` (in_len bytes) into a freshly malloc'd gzip stream. On success
 * returns 0 and sets *out (caller frees) and *out_len. Returns -1 on failure
 * (out is set to NULL). */
int ph_gzip(const char *in, size_t in_len, char **out, size_t *out_len);

#endif /* PH_GZIP_H */
