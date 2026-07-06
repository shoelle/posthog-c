/*
 * ph_http.h — the native HTTP transport.
 *
 * http:// goes over a plaintext socket (Winsock/POSIX) — handy for a local dev
 * proxy. https:// delegates to the TLS backend in ph_tls.c (WinHTTP on
 * Windows). The plaintext request *builder* is split from the socket send so
 * the wire format can be unit-tested without a live server.
 */
#ifndef PH_HTTP_H
#define PH_HTTP_H

#include "ph_internal.h"
#include "ph_str.h"

#include <stddef.h>

/* Create the default transport (stateless; POSTs application/json). */
ph_transport ph_http_transport_create(void);

/* Build the raw HTTP/1.1 request bytes for POSTing `body` to `url` into `out`.
 * Pure and testable. Returns PH_OK, or PH_ERR for a malformed/unsupported URL
 * (e.g. https:// before TLS exists). */
ph_result ph_http_build_request(const char *url, const char *body,
                                size_t body_len, ph_strbuf *out);

#endif /* PH_HTTP_H */
