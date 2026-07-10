#include "ph_http.h"
#include "ph_gzip.h"
#include "ph_tls.h"
#include "ph_time.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ph_socket;
#define PH_INVALID_SOCK INVALID_SOCKET
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int ph_socket;
#define PH_INVALID_SOCK (-1)
#endif

/* --- URL parsing ------------------------------------------------------ */

typedef struct ph_url {
    char host[PH_HOST_CAP];
    char port[8];
    char path[PH_HOST_CAP];
    int is_https;
} ph_url;

/* Parse "http://host[:port]/path". Returns PH_OK, PH_ERR on malformed input.
 * is_https is reported so the caller can refuse it until TLS lands. */
static ph_result parse_url(const char *url, ph_url *out) {
    const char *p = url;
    const char *host_start;
    const char *host_end;
    const char *colon;
    size_t hlen;

    memset(out, 0, sizeof(*out));
    strcpy(out->port, "80");
    strcpy(out->path, "/");

    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        out->is_https = 0;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        out->is_https = 1;
        strcpy(out->port, "443");
    } else {
        return PH_ERR;
    }

    host_start = p;
    while (*p && *p != '/' && *p != ':') p++;
    host_end = p;
    hlen = (size_t)(host_end - host_start);
    if (hlen == 0 || hlen >= sizeof(out->host)) return PH_ERR;
    memcpy(out->host, host_start, hlen);
    out->host[hlen] = '\0';

    if (*p == ':') {
        size_t i = 0;
        p++;
        colon = p;
        while (*p && *p != '/') p++;
        if ((size_t)(p - colon) >= sizeof(out->port)) return PH_ERR;
        for (i = 0; colon + i < p; i++) out->port[i] = colon[i];
        out->port[i] = '\0';
    }

    if (*p == '/') {
        if (strlen(p) >= sizeof(out->path)) return PH_ERR;
        strcpy(out->path, p);
    }
    return PH_OK;
}

ph_result ph_http_build_request(const char *url, const char *body,
                                size_t body_len, const char *content_encoding,
                                ph_strbuf *out) {
    ph_url u;
    char line[PH_HOST_CAP + 64];
    if (parse_url(url, &u) != PH_OK) return PH_ERR;
    if (u.is_https) return PH_ERR; /* https is handled by the TLS backend */

    snprintf(line, sizeof(line), "POST %s HTTP/1.1\r\n", u.path);
    ph_strbuf_append_cstr(out, line);
    /* Host header carries the port when non-default, mirroring what proxies
     * expect. */
    if (strcmp(u.port, "80") == 0)
        snprintf(line, sizeof(line), "Host: %s\r\n", u.host);
    else
        snprintf(line, sizeof(line), "Host: %s:%s\r\n", u.host, u.port);
    ph_strbuf_append_cstr(out, line);
    ph_strbuf_append_cstr(out, "Content-Type: application/json\r\n");
    ph_strbuf_append_cstr(out, "User-Agent: posthog-c/" PH_VERSION_STRING "\r\n");
    if (content_encoding && content_encoding[0]) {
        snprintf(line, sizeof(line), "Content-Encoding: %s\r\n", content_encoding);
        ph_strbuf_append_cstr(out, line);
    }
    snprintf(line, sizeof(line), "Content-Length: %lu\r\n", (unsigned long)body_len);
    ph_strbuf_append_cstr(out, line);
    ph_strbuf_append_cstr(out, "Connection: close\r\n\r\n");
    ph_strbuf_append(out, body, body_len);
    return PH_OK;
}

/* --- Socket send ------------------------------------------------------ */

#if defined(_WIN32)
static void winsock_once(void) {
    static int done = 0;
    if (!done) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = 1; /* left running for process lifetime; cheap and simplifies teardown */
    }
}
static void sock_close(ph_socket s) { closesocket(s); }
#else
static void winsock_once(void) {}
static void sock_close(ph_socket s) { close(s); }
#endif

static void set_timeouts(ph_socket s, int timeout_ms) {
    if (timeout_ms <= 0) return;
#if defined(_WIN32)
    {
        DWORD tv = (DWORD)timeout_ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
    }
#else
    {
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
#endif
}

static uint64_t request_deadline(int timeout_ms) {
    uint64_t now;
    uint64_t add;
    if (timeout_ms <= 0) return 0;
    now = ph_now_mono_ns();
    add = (uint64_t)timeout_ms * 1000000ull;
    return UINT64_MAX - now < add ? UINT64_MAX : now + add;
}

static int remaining_ms(uint64_t deadline) {
    uint64_t now, left, ms;
    if (deadline == 0) return -1;
    now = ph_now_mono_ns();
    if (now >= deadline) return 0;
    left = deadline - now;
    ms = (left + 999999ull) / 1000000ull;
    return ms > (uint64_t)INT_MAX ? INT_MAX : (int)ms;
}

static int set_nonblocking(ph_socket s, int enabled) {
#if defined(_WIN32)
    u_long mode = enabled ? 1ul : 0ul;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return 0;
    if (enabled) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    return fcntl(s, F_SETFL, flags) == 0;
#endif
}

static int connect_pending(void) {
#if defined(_WIN32)
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EINPROGRESS || errno == EWOULDBLOCK;
#endif
}

static int connect_until(ph_socket s, const struct sockaddr *addr, int addrlen,
                         uint64_t deadline) {
    fd_set writefds, exceptfds;
    struct timeval tv;
    struct timeval *tvp = NULL;
    int ms, rc, error = 0;
    socklen_t error_len = (socklen_t)sizeof(error);

    if (!deadline) return connect(s, addr, addrlen) == 0;
    if (!set_nonblocking(s, 1)) return 0;
    rc = connect(s, addr, addrlen);
    if (rc == 0) {
        set_nonblocking(s, 0);
        return 1;
    }
    if (!connect_pending()) {
        set_nonblocking(s, 0);
        return 0;
    }
    ms = remaining_ms(deadline);
    if (ms <= 0) {
        set_nonblocking(s, 0);
        return 0;
    }
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    tvp = &tv;
    FD_ZERO(&writefds);
    FD_ZERO(&exceptfds);
    FD_SET(s, &writefds);
    FD_SET(s, &exceptfds);
    rc = select((int)s + 1, NULL, &writefds, &exceptfds, tvp);
    if (rc > 0 && FD_ISSET(s, &writefds) &&
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&error, &error_len) == 0 &&
        error == 0) {
        set_nonblocking(s, 0);
        return 1;
    }
    set_nonblocking(s, 0);
    return 0;
}

/* Parse "HTTP/1.1 200 OK" -> 200. Returns -1 if unrecognized. */
static int parse_status(const char *resp, size_t n) {
    size_t i = 0;
    while (i < n && resp[i] != ' ') i++; /* skip "HTTP/1.1" */
    while (i < n && resp[i] == ' ') i++;
    if (i + 3 > n) return -1;
    if (resp[i] < '0' || resp[i] > '9' ||
        resp[i + 1] < '0' || resp[i + 1] > '9' ||
        resp[i + 2] < '0' || resp[i + 2] > '9')
        return -1;
    return (resp[i] - '0') * 100 + (resp[i + 1] - '0') * 10 + (resp[i + 2] - '0');
}

static const char *find_header_end(const char *buf, size_t n) {
    size_t i;
    for (i = 0; i + 3 < n; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return buf + i;
    }
    return NULL;
}

/* Copy the value of a "Retry-After:" header (case-insensitive, matched at a
 * line start) out of a raw response head into `out` (trimmed, NUL-terminated).
 * `out[0]` is set to '\0' when the header is absent. */
static void scan_retry_after(const char *buf, size_t n, char *out, size_t cap) {
    static const char key[] = "retry-after:";
    const size_t klen = sizeof(key) - 1;
    size_t i;
    if (!out || cap == 0) return;
    out[0] = '\0';
    for (i = 0; i + klen <= n; i++) {
        size_t k = 0;
        if (i != 0 && buf[i - 1] != '\n') continue; /* header names start a line */
        while (k < klen && (char)tolower((unsigned char)buf[i + k]) == key[k]) k++;
        if (k == klen) {
            size_t j = i + klen, o = 0;
            while (j < n && (buf[j] == ' ' || buf[j] == '\t')) j++;
            while (j < n && buf[j] != '\r' && buf[j] != '\n' && o + 1 < cap)
                out[o++] = buf[j++];
            out[o] = '\0';
            return;
        }
    }
}

/* ~100 MB Content-Length sanity ceiling: our responses are tiny, and this keeps
 * the length accumulation from overflowing on a hostile header. */
#define PH_HTTP_MAX_CONTENT_LEN 100000000L
static long scan_content_length(const char *buf, size_t n) {
    static const char key[] = "content-length:";
    const size_t klen = sizeof(key) - 1;
    size_t i;
    for (i = 0; i + klen <= n; i++) {
        size_t k = 0;
        long len = 0;
        if (i != 0 && buf[i - 1] != '\n') continue;
        while (k < klen && (char)tolower((unsigned char)buf[i + k]) == key[k]) k++;
        if (k == klen) {
            size_t j = i + klen;
            while (j < n && (buf[j] == ' ' || buf[j] == '\t')) j++;
            if (j >= n || buf[j] < '0' || buf[j] > '9') return -1;
            while (j < n && buf[j] >= '0' && buf[j] <= '9') {
                if (len > PH_HTTP_MAX_CONTENT_LEN) return PH_HTTP_MAX_CONTENT_LEN;
                len = len * 10 + (long)(buf[j] - '0');
                j++;
            }
            return len;
        }
    }
    return -1;
}

/* True if a "Transfer-Encoding:" header lists the "chunked" coding. Matches the
 * header name at a line start (case-insensitive) and scans its comma-separated
 * token list, so "gzip, chunked" also counts. */
static int scan_transfer_chunked(const char *buf, size_t n) {
    static const char key[] = "transfer-encoding:";
    static const char token[] = "chunked";
    const size_t klen = sizeof(key) - 1;
    const size_t tlen = sizeof(token) - 1;
    size_t i;

    for (i = 0; i + klen <= n; i++) {
        size_t k = 0;
        if (i != 0 && buf[i - 1] != '\n') continue;
        while (k < klen && (char)tolower((unsigned char)buf[i + k]) == key[k]) k++;
        if (k == klen) {
            size_t j = i + klen;
            while (j < n && buf[j] != '\r' && buf[j] != '\n') {
                size_t start, len, m;
                while (j < n && (buf[j] == ' ' || buf[j] == '\t' || buf[j] == ','))
                    j++;
                start = j;
                while (j < n && buf[j] != ',' && buf[j] != '\r' &&
                       buf[j] != '\n' && buf[j] != ' ' && buf[j] != '\t')
                    j++;
                len = j - start;
                if (len == tlen) {
                    for (m = 0; m < tlen; m++) {
                        if ((char)tolower((unsigned char)buf[start + m]) != token[m])
                            break;
                    }
                    if (m == tlen) return 1;
                }
                while (j < n && buf[j] != ',' && buf[j] != '\r' && buf[j] != '\n')
                    j++;
            }
            return 0;
        }
    }
    return 0;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode an HTTP chunked body into a bounded prefix buffer: for each
 * "<hexlen> CRLF <data> CRLF" chunk, append data (NUL-terminated, capped at
 * out_cap) until the 0-length terminator, a chunk that runs past the bytes on
 * hand, or the cap. Sets *complete iff the terminating 0-chunk was reached.
 * Returns bytes copied. */
static size_t copy_chunked_prefix(const char *body, size_t body_len,
                                  char *out, size_t out_cap, int *complete) {
    size_t pos = 0, copied = 0;
    if (complete) *complete = 0;
    if (out && out_cap) out[0] = '\0';

    while (pos < body_len) {
        size_t chunk_len = 0, avail, take;
        int digits = 0;

        while (pos < body_len && (body[pos] == ' ' || body[pos] == '\t')) pos++;
        while (pos < body_len) {
            int hv = hex_value(body[pos]);
            if (hv < 0) break;
            /* accumulate hex digits, saturating at SIZE_MAX rather than wrapping
             * on a hostile/oversized chunk length */
            if (chunk_len <= ((size_t)-1 - (size_t)hv) / 16)
                chunk_len = chunk_len * 16 + (size_t)hv;
            else
                chunk_len = (size_t)-1;
            pos++;
            digits = 1;
        }
        if (!digits) break;

        while (pos < body_len && body[pos] != '\n') pos++;
        if (pos >= body_len) break;
        pos++;

        if (chunk_len == 0) {
            if (complete) *complete = 1;
            break;
        }

        avail = body_len - pos;
        take = avail < chunk_len ? avail : chunk_len;
        if (out && out_cap && copied + 1 < out_cap) {
            size_t room = out_cap - 1 - copied;
            size_t copy = take < room ? take : room;
            if (copy) {
                memcpy(out + copied, body + pos, copy);
                copied += copy;
                out[copied] = '\0';
            }
        }
        if (out_cap && copied + 1 >= out_cap) break;
        if (avail < chunk_len) break;

        pos += chunk_len;
        if (pos >= body_len) break;
        if (body[pos] == '\r') {
            if (pos + 1 >= body_len) break;
            if (body[pos + 1] != '\n') break;
            pos += 2;
        } else if (body[pos] == '\n') {
            pos++;
        } else {
            break;
        }
    }
    return copied;
}

static void copy_body_prefix(const char *body, size_t body_len, int chunked,
                             char *out, size_t out_cap, int *complete) {
    if (complete) *complete = 0;
    if (!out || out_cap == 0) return;
    out[0] = '\0';
    if (chunked) {
        copy_chunked_prefix(body, body_len, out, out_cap, complete);
    } else {
        size_t copy = body_len < out_cap - 1 ? body_len : out_cap - 1;
        if (copy) memcpy(out, body, copy);
        out[copy] = '\0';
        if (complete) *complete = 1;
    }
}

/* Strict full-body chunk decoder. Return 1 when the zero chunk and trailers are
 * complete, 0 when more bytes are needed, -1 for malformed framing, and -2
 * when the decoded bytes do not fit. `out == NULL` performs framing only. */
static int decode_chunked_full(const char *body, size_t body_len,
                               char *out, size_t out_cap, size_t *out_len) {
    size_t pos = 0, copied = 0;
    if (out && out_cap) out[0] = '\0';
    if (out_len) *out_len = 0;

    for (;;) {
        size_t chunk_len = 0;
        int digits = 0;
        while (pos < body_len) {
            int hv = hex_value(body[pos]);
            if (hv < 0) break;
            if (chunk_len > ((size_t)-1 - (size_t)hv) / 16) return -1;
            chunk_len = chunk_len * 16 + (size_t)hv;
            digits = 1;
            pos++;
        }
        if (!digits) return pos == body_len ? 0 : -1;
        /* Chunk extensions are permitted but the size line must end in CRLF. */
        while (pos < body_len && body[pos] != '\r' && body[pos] != '\n') pos++;
        if (pos + 1 >= body_len) return 0;
        if (body[pos] != '\r' || body[pos + 1] != '\n') return -1;
        pos += 2;

        if (chunk_len == 0) {
            /* Consume optional trailer fields through their terminating blank
             * line. We do not expose trailer values. */
            for (;;) {
                size_t line = pos;
                while (pos < body_len && body[pos] != '\r' && body[pos] != '\n') pos++;
                if (pos + 1 >= body_len) return 0;
                if (body[pos] != '\r' || body[pos + 1] != '\n') return -1;
                pos += 2;
                if (pos == line + 2) {
                    if (out && out_cap) out[copied] = '\0';
                    if (out_len) *out_len = copied;
                    return 1;
                }
            }
        }

        if (chunk_len > body_len - pos) return 0;
        if (chunk_len > (size_t)-1 - copied) return -1;
        if (out && (out_cap == 0 || copied >= out_cap ||
                    chunk_len >= out_cap - copied))
            return -2;
        if (out && chunk_len) memcpy(out + copied, body + pos, chunk_len);
        copied += chunk_len;
        pos += chunk_len;
        if (pos + 1 >= body_len) return 0;
        if (body[pos] != '\r' || body[pos + 1] != '\n') return -1;
        pos += 2;
    }
}

int ph__http_decode_response_body(const char *resp, size_t resp_len,
                                  char *out, size_t out_cap) {
    const char *sep;
    const char *body;
    size_t head_len, body_len;
    long content_len;
    int status, chunk_rc;

    if (out && out_cap) out[0] = '\0';
    if (!resp) return PH_HTTP_RESPONSE_TRUNCATED;
    sep = find_header_end(resp, resp_len);
    if (!sep) return PH_HTTP_RESPONSE_TRUNCATED;
    status = parse_status(resp, (size_t)(sep - resp));
    if (status < 0) return PH_HTTP_RESPONSE_TRUNCATED;
    head_len = (size_t)(sep - resp);
    body = sep + 4;
    body_len = resp_len - (size_t)(body - resp);

    if (scan_transfer_chunked(resp, head_len)) {
        chunk_rc = decode_chunked_full(body, body_len, out, out_cap, NULL);
        if (chunk_rc == -2) return PH_HTTP_RESPONSE_TOO_LARGE;
        return chunk_rc == 1 ? status : PH_HTTP_RESPONSE_TRUNCATED;
    }

    content_len = scan_content_length(resp, head_len);
    if (content_len >= 0) {
        if ((size_t)content_len > body_len) return PH_HTTP_RESPONSE_TRUNCATED;
        body_len = (size_t)content_len;
    }
    if (body_len > 0 && (!out || out_cap == 0 || body_len >= out_cap))
        return PH_HTTP_RESPONSE_TOO_LARGE;
    if (body_len) memcpy(out, body, body_len);
    if (out && out_cap) out[body_len] = '\0';
    return status;
}

int ph__http_parse_response_meta(const char *resp, size_t resp_len,
                                 ph_send_meta *meta) {
    const char *sep = find_header_end(resp, resp_len);
    size_t head_len = sep ? (size_t)(sep - resp) : resp_len;
    int status = parse_status(resp, resp_len);

    if (meta) {
        meta->body[0] = '\0';
        scan_retry_after(resp, head_len, meta->retry_after,
                         sizeof meta->retry_after);
        if (sep) {
            const char *b = sep + 4;
            size_t blen = resp_len - (size_t)(b - resp);
            int chunked = scan_transfer_chunked(resp, head_len);
            copy_body_prefix(b, blen, chunked, meta->body, sizeof meta->body, NULL);
        }
    }
    return status;
}

/* True once enough of the response is in hand to stop reading (rather than wait
 * for the peer to close): the 0-chunk for chunked, Content-Length bytes when
 * given, else any body at all. Either path also stops once the bounded prefix
 * fills - we only need the status and the small quota notice, not the body. */
int ph__http_send_response_complete(const char *resp, size_t resp_len) {
    const char *sep = find_header_end(resp, resp_len);
    long content_len;
    size_t head_len, body_have;

    if (!sep) return 0;
    head_len = (size_t)(sep - resp);
    body_have = resp_len - (size_t)((sep + 4) - resp);

    if (scan_transfer_chunked(resp, head_len)) {
        char prefix[PH_RESP_BODY_CAP];
        int chunked_complete = 0;
        size_t prefix_len = copy_chunked_prefix(sep + 4, body_have, prefix,
                                                sizeof prefix, &chunked_complete);
        return chunked_complete || prefix_len >= PH_RESP_BODY_CAP - 1;
    }

    content_len = scan_content_length(resp, head_len);
    if (content_len >= 0)
        return body_have >= (size_t)content_len ||
               body_have >= PH_RESP_BODY_CAP - 1;

    return body_have >= PH_RESP_BODY_CAP - 1 || body_have > 0;
}

/* Response-buffer sizes: the status-only path needs room for the status line,
 * headers, and the small quota-notice prefix; the body path reads in chunks. */
#define PH_HTTP_STATUS_BUF 4096
#define PH_HTTP_FETCH_CHUNK 2048
#define PH_HTTP_HEADER_CAP 16384
#define PH_HTTP_CHUNK_OVERHEAD_CAP 16384

/* Read a small status-only response: status line + headers (Retry-After) + the
 * body prefix carrying a quota notice. Stops at Content-Length, a complete
 * chunked body, or the buffer cap. Returns the HTTP status, or -1. */
static int read_status_response(ph_socket s, ph_send_meta *meta,
                                uint64_t deadline) {
    char resp[PH_HTTP_STATUS_BUF];
    size_t total = 0;
    for (;;) {
        int ms = remaining_ms(deadline);
        int n;
        if (deadline && ms <= 0) return -1;
        if (ms > 0) set_timeouts(s, ms);
        n = (int)recv(s, resp + total, (int)(sizeof(resp) - 1 - total), 0);
        if (n <= 0) break;
        total += (size_t)n;
        resp[total] = '\0';
        if (ph__http_send_response_complete(resp, total)) break;
        if (total >= sizeof(resp) - 1) break;
    }
    return total > 0 ? ph__http_parse_response_meta(resp, total, meta) : -1;
}

/* Read the whole response, parse the status, and copy the body (past the
 * "\r\n\r\n" terminator) into out (NUL-terminated, capped) - used for /flags/.
 * Returns the HTTP status, or -1. */
static int read_body_response(ph_socket s, char *out, size_t out_cap,
                              uint64_t deadline) {
    char *resp;
    size_t total = 0, raw_cap;
    int closed = 0, framed = 0;
    int result = PH_HTTP_RESPONSE_TRUNCATED;

    if (out_cap > (size_t)-1 - PH_HTTP_HEADER_CAP - PH_HTTP_CHUNK_OVERHEAD_CAP)
        return PH_HTTP_RESPONSE_TOO_LARGE;
    raw_cap = out_cap + PH_HTTP_HEADER_CAP + PH_HTTP_CHUNK_OVERHEAD_CAP;
    resp = (char *)malloc(raw_cap + 1);
    if (!resp) return -1;
    for (;;) {
        const char *sep;
        size_t room = raw_cap - total;
        int ms = remaining_ms(deadline);
        int n;
        if (deadline && ms <= 0) break;
        if (room == 0) {
            result = PH_HTTP_RESPONSE_TOO_LARGE;
            goto done;
        }
        if (ms > 0) set_timeouts(s, ms);
        if (room > PH_HTTP_FETCH_CHUNK) room = PH_HTTP_FETCH_CHUNK;
        n = (int)recv(s, resp + total, (int)room, 0);
        if (n == 0) {
            closed = 1;
            break;
        }
        if (n < 0) break;
        total += (size_t)n;
        resp[total] = '\0';
        sep = find_header_end(resp, total);
        if (!sep) {
            if (total >= PH_HTTP_HEADER_CAP) break;
        } else {
            size_t head_len = (size_t)(sep - resp);
            size_t body_have = total - (size_t)((sep + 4) - resp);
            long content_len;
            if (head_len > PH_HTTP_HEADER_CAP) break;
            if (scan_transfer_chunked(resp, head_len)) {
                int chunk_rc = decode_chunked_full(sep + 4, body_have,
                                                   NULL, 0, NULL);
                framed = 1;
                if (chunk_rc == 1) break;
                if (chunk_rc < 0) goto done;
            } else {
                content_len = scan_content_length(resp, head_len);
                if (content_len >= 0) {
                    framed = 1;
                    if ((size_t)content_len >= out_cap) {
                        result = PH_HTTP_RESPONSE_TOO_LARGE;
                        goto done;
                    }
                    if (body_have >= (size_t)content_len) break;
                }
            }
        }
    }
    /* Close-delimited bodies are complete only at EOF; Content-Length and
     * chunked bodies can complete without waiting for the peer to close. */
    if (closed || framed)
        result = ph__http_decode_response_body(resp, total, out, out_cap);
done:
    free(resp);
    return result;
}

/* Core native HTTP. If `out` is non-NULL the response *body* (after the header
 * terminator) is copied into it (NUL-terminated, capped) - used for /flags/;
 * otherwise only the status line is read (the capture path discards the body).
 * Returns the HTTP status, or <0 on failure. */
static int do_http(const char *url, const char *body, size_t body_len,
                   int timeout_ms, char *out, size_t out_cap,
                   const char *content_encoding, ph_send_meta *meta) {
    ph_url u;
    ph_strbuf req;
    struct addrinfo hints, *res = NULL, *ai;
    ph_socket s = PH_INVALID_SOCK;
    int status = -1;
    size_t sent = 0;
    uint64_t deadline = request_deadline(timeout_ms);

    if (out && out_cap) out[0] = '\0';
    if (parse_url(url, &u) != PH_OK) return -1;

    /* https delegates to the TLS backend (WinHTTP on Windows). */
    if (u.is_https)
        return out ? ph_tls_fetch(u.host, atoi(u.port), u.path, body, body_len,
                                  timeout_ms, out, out_cap)
                   : ph_tls_send(u.host, atoi(u.port), u.path, body, body_len,
                                 timeout_ms, content_encoding,
                                 meta ? meta->retry_after : NULL,
                                 meta ? sizeof meta->retry_after : 0,
                                 meta ? meta->body : NULL,
                                 meta ? sizeof meta->body : 0);

    winsock_once();

    ph_strbuf_init(&req);
    if (ph_http_build_request(url, body, body_len, content_encoding, &req) != PH_OK ||
        req.oom) {
        ph_strbuf_free(&req);
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u.host, u.port, &hints, &res) != 0) {
        ph_strbuf_free(&req);
        return -1;
    }
    /* getaddrinfo is synchronous on the supported libc/Winsock APIs. Its time
     * counts against the deadline, but cannot be interrupted portably. */
    if (deadline && remaining_ms(deadline) <= 0) {
        freeaddrinfo(res);
        ph_strbuf_free(&req);
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        int ms = remaining_ms(deadline);
        if (deadline && ms <= 0) break;
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == PH_INVALID_SOCK) continue;
        if (connect_until(s, ai->ai_addr, (int)ai->ai_addrlen, deadline)) break;
        sock_close(s);
        s = PH_INVALID_SOCK;
    }
    freeaddrinfo(res);
    if (s == PH_INVALID_SOCK) {
        ph_strbuf_free(&req);
        return -1;
    }
    while (sent < req.len) {
        int ms = remaining_ms(deadline);
        if (deadline && ms <= 0) {
            sock_close(s);
            ph_strbuf_free(&req);
            return -1;
        }
        if (ms > 0) set_timeouts(s, ms);
        int n = (int)send(s, req.data + sent, (int)(req.len - sent), 0);
        if (n <= 0) {
            sock_close(s);
            ph_strbuf_free(&req);
            return -1;
        }
        sent += (size_t)n;
    }

    status = out ? read_body_response(s, out, out_cap, deadline)
                 : read_status_response(s, meta, deadline);

    sock_close(s);
    ph_strbuf_free(&req);
    return status;
}

static int http_send(void *self, const char *url, const char *body,
                     size_t body_len, int timeout_ms, ph_send_meta *meta) {
    char *gz = NULL;
    size_t gzlen = 0;
    const char *enc = NULL;
    int rc;
    (void)self;
    /* gzip the batch body when enabled; fall back to plaintext on failure. */
    if (g_ph.gzip && body_len > 0 && ph_gzip(body, body_len, &gz, &gzlen) == 0) {
        body = gz;
        body_len = gzlen;
        enc = "gzip";
    }
    rc = do_http(url, body, body_len, timeout_ms, NULL, 0, enc, meta);
    free(gz);
    return rc;
}

static int http_fetch(void *self, const char *url, const char *body,
                      size_t body_len, int timeout_ms, char *out, size_t out_cap) {
    (void)self;
    return do_http(url, body, body_len, timeout_ms, out, out_cap, NULL, NULL);
}

ph_transport ph_http_transport_create(void) {
    ph_transport t;
    t.send = http_send;
    t.fetch = http_fetch;
    t.destroy = NULL;
    t.self = NULL;
    return t;
}
