#include "ph_http.h"
#include "ph_gzip.h"
#include "ph_tls.h"

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

/* Parse "HTTP/1.1 200 OK" -> 200. Returns -1 if unrecognized. */
static int parse_status(const char *resp, size_t n) {
    size_t i = 0;
    while (i < n && resp[i] != ' ') i++; /* skip "HTTP/1.1" */
    while (i < n && resp[i] == ' ') i++;
    if (i + 3 > n) return -1;
    if (resp[i] < '0' || resp[i] > '9') return -1;
    return (resp[i] - '0') * 100 + (resp[i + 1] - '0') * 10 + (resp[i + 2] - '0');
}

/* Core native HTTP. If `out` is non-NULL the response *body* (after the header
 * terminator) is copied into it (NUL-terminated, capped) — used for /flags/;
 * otherwise only the status line is read (the capture path discards the body).
 * Returns the HTTP status, or <0 on failure. */
static int do_http(const char *url, const char *body, size_t body_len,
                   int timeout_ms, char *out, size_t out_cap,
                   const char *content_encoding) {
    ph_url u;
    ph_strbuf req;
    struct addrinfo hints, *res = NULL, *ai;
    ph_socket s = PH_INVALID_SOCK;
    int status = -1;
    size_t sent = 0;

    if (out && out_cap) out[0] = '\0';
    if (parse_url(url, &u) != PH_OK) return -1;

    /* https delegates to the TLS backend (WinHTTP on Windows). */
    if (u.is_https)
        return out ? ph_tls_fetch(u.host, atoi(u.port), u.path, body, body_len,
                                  timeout_ms, out, out_cap)
                   : ph_tls_send(u.host, atoi(u.port), u.path, body, body_len,
                                 timeout_ms, content_encoding);

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
    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == PH_INVALID_SOCK) continue;
        set_timeouts(s, timeout_ms);
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        sock_close(s);
        s = PH_INVALID_SOCK;
    }
    freeaddrinfo(res);
    if (s == PH_INVALID_SOCK) {
        ph_strbuf_free(&req);
        return -1;
    }
    while (sent < req.len) {
        int n = (int)send(s, req.data + sent, (int)(req.len - sent), 0);
        if (n <= 0) {
            sock_close(s);
            ph_strbuf_free(&req);
            return -1;
        }
        sent += (size_t)n;
    }

    if (!out) {
        char head[256];
        int n = (int)recv(s, head, (int)sizeof(head) - 1, 0);
        if (n > 0) {
            head[n] = '\0';
            status = parse_status(head, (size_t)n);
        }
    } else {
        /* Read the whole response, parse the status, copy the body past the
         * "\r\n\r\n" header terminator. */
        ph_strbuf resp;
        ph_strbuf_init(&resp);
        for (;;) {
            char chunk[2048];
            int n = (int)recv(s, chunk, (int)sizeof(chunk), 0);
            if (n <= 0) break;
            ph_strbuf_append(&resp, chunk, (size_t)n);
            if (resp.oom) break;
        }
        if (resp.data) {
            const char *sep = strstr(resp.data, "\r\n\r\n");
            status = parse_status(resp.data, resp.len);
            if (sep && out_cap > 0) {
                const char *b = sep + 4;
                size_t blen = resp.len - (size_t)(b - resp.data);
                size_t copy = blen < out_cap - 1 ? blen : out_cap - 1;
                memcpy(out, b, copy);
                out[copy] = '\0';
            }
        }
        ph_strbuf_free(&resp);
    }

    sock_close(s);
    ph_strbuf_free(&req);
    return status;
}

static int http_send(void *self, const char *url, const char *body,
                     size_t body_len, int timeout_ms) {
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
    rc = do_http(url, body, body_len, timeout_ms, NULL, 0, enc);
    free(gz);
    return rc;
}

static int http_fetch(void *self, const char *url, const char *body,
                      size_t body_len, int timeout_ms, char *out, size_t out_cap) {
    (void)self;
    return do_http(url, body, body_len, timeout_ms, out, out_cap, NULL);
}

ph_transport ph_http_transport_create(void) {
    ph_transport t;
    t.send = http_send;
    t.fetch = http_fetch;
    t.destroy = NULL;
    t.self = NULL;
    return t;
}
