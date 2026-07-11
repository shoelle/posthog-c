/*
 * ph_http.h - the native HTTP transport.
 *
 * http:// goes over a plaintext socket (Winsock/POSIX) - handy for a local dev
 * proxy. https:// delegates to the system TLS backend in ph_tls.c. The plaintext
 * request *builder* is split from the socket send so the wire format can be
 * unit-tested without a live server.
 */
#ifndef PH_HTTP_H
#define PH_HTTP_H

#include "ph_internal.h"
#include "ph_str.h"

#include <stddef.h>

/* Create the default transport (stateless; POSTs application/json). */
ph_transport ph_http_transport_create(void);

/* Build the raw HTTP/1.1 request bytes for POSTing `body` to `url` into `out`.
 * `content_encoding` adds a Content-Encoding header when non-NULL (e.g. "gzip").
 * Pure and testable. Returns PH_OK, or PH_ERR for a malformed/unsupported URL
 * (e.g. https://, which goes through the TLS backend). */
ph_result ph_http_build_request(const char *url, const char *body,
                                size_t body_len, const char *content_encoding,
                                ph_strbuf *out);

/* Parse the bounded response prefix used by the send path. Internal/testable:
 * returns the HTTP status and fills Retry-After plus a decoded body prefix. */
int ph__http_parse_response_meta(const char *resp, size_t resp_len,
                                 ph_send_meta *meta);

/* True once the send path has enough response bytes to stop reading without
 * waiting for connection close. */
int ph__http_send_response_complete(const char *resp, size_t resp_len);

/* True once the fetch/body path has enough bytes to stop reading without
 * waiting for connection close. Close-delimited bodies only complete at EOF, so
 * this only returns true for framed responses (Content-Length or chunked), plus
 * terminal framing errors. */
int ph__http_body_response_complete(const char *resp, size_t resp_len);

/* Decode a complete HTTP response into a bounded body buffer. Chunked transfer
 * coding is removed and Content-Length is enforced. Returns the HTTP status,
 * PH_HTTP_RESPONSE_TRUNCATED for malformed/incomplete framing, or
 * PH_HTTP_RESPONSE_TOO_LARGE when the decoded body does not fit. */
#define PH_HTTP_RESPONSE_TRUNCATED (-1)
#define PH_HTTP_RESPONSE_TOO_LARGE (-2)
int ph__http_decode_response_body(const char *resp, size_t resp_len,
                                  char *out, size_t out_cap);

#endif /* PH_HTTP_H */
