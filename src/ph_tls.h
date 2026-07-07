/*
 * ph_tls.h — the HTTPS side of the native transport (v0.2).
 *
 * https:// URLs route here from ph_http.c. On Windows we reuse WinHTTP, which
 * is always present and validates the server certificate against the OS trust
 * store — so on Windows we "reuse what the host has" rather than shipping a
 * second TLS stack. macOS (Secure Transport) and Linux (a vendored BearSSL)
 * backends slot in behind this same call; until then they return an error.
 */
#ifndef PH_TLS_H
#define PH_TLS_H

#include <stddef.h>

/* POST `body` to https://host:port/path. `content_encoding` adds a
 * Content-Encoding header when non-NULL (e.g. "gzip"). When `retry_after` is
 * non-NULL, the response's Retry-After header value is copied into it
 * (NUL-terminated, capped at retry_after_cap; "" if absent); when `body_out` is
 * non-NULL, a prefix of the response body is copied into it (capped at
 * body_out_cap) for the quota-limit notice. Both feed the sender's rate limiter.
 * Returns the HTTP status code, or a value < 0 on a connect/TLS/transport
 * failure. Blocks up to timeout_ms. */
int ph_tls_send(const char *host, int port, const char *path, const char *body,
                size_t body_len, int timeout_ms, const char *content_encoding,
                char *retry_after, size_t retry_after_cap, char *body_out,
                size_t body_out_cap);

/* Like ph_tls_send, but also copy the response body into out (NUL-terminated,
 * capped at out_cap) — used for /flags/. */
int ph_tls_fetch(const char *host, int port, const char *path, const char *body,
                 size_t body_len, int timeout_ms, char *out, size_t out_cap);

/* 1 if this build has a working TLS backend for the current platform. */
int ph_tls_available(void);

#endif /* PH_TLS_H */
