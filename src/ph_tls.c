#include "ph_tls.h"
#include "ph_time.h"
#include "posthog.h"

#include <limits.h>

/* Absolute monotonic deadline `timeout_ms` from now (0 = no deadline). */
static uint64_t tls_deadline(int timeout_ms) {
    uint64_t now, add;
    if (timeout_ms <= 0) return 0;
    now = ph_now_mono_ns();
    add = (uint64_t)timeout_ms * 1000000ull;
    return UINT64_MAX - now < add ? UINT64_MAX : now + add;
}

/* Milliseconds left until `deadline` (-1 = no deadline, 0 = expired). */
static int tls_remaining_ms(uint64_t deadline) {
    uint64_t now, left, ms;
    if (!deadline) return -1;
    now = ph_now_mono_ns();
    if (now >= deadline) return 0;
    left = deadline - now;
    ms = (left + 999999ull) / 1000000ull;
    return ms > (uint64_t)INT_MAX ? INT_MAX : (int)ms;
}

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

static int to_wide(const char *s, wchar_t *out, int cap) {
    /* Returns chars written incl. NUL, or 0 on failure. */
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
}

int ph_tls_available(void) { return 1; }

/* Shared WinHTTP POST. When `out` is non-NULL the response body is read into it
 * (NUL-terminated, capped). Returns the HTTP status, or <0 on failure. */
static int do_tls(const char *host, int port, const char *path, const char *body,
                  size_t body_len, int timeout_ms, char *out, size_t out_cap,
                  const char *content_encoding, char *retry_after,
                  size_t retry_after_cap, int require_full_body) {
    wchar_t whost[256];
    wchar_t wpath[1024];
    wchar_t wagent[64];
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD status = 0;
    DWORD sz = sizeof(status);
    uint64_t deadline = tls_deadline(timeout_ms);
    int phase_ms = timeout_ms > 0 ? timeout_ms / 4 : 0;
    int rc = -1;

    if (phase_ms == 0 && timeout_ms > 0) phase_ms = 1;

    if (out && out_cap) out[0] = '\0';
    if (retry_after && retry_after_cap) retry_after[0] = '\0';
    if (!to_wide(host, whost, 256)) return -1;
    if (!to_wide((path && path[0]) ? path : "/", wpath, 1024)) return -1;
    if (!to_wide("posthog-c/" PH_VERSION_STRING, wagent, 64)) return -1;

    ses = WinHttpOpen(wagent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) goto done;
    if (phase_ms > 0)
        WinHttpSetTimeouts(ses, phase_ms, phase_ms, phase_ms, phase_ms);

    con = WinHttpConnect(ses, whost, (INTERNET_PORT)port, 0);
    if (!con) goto done;

    /* WINHTTP_FLAG_SECURE enables TLS; the server cert is validated against the
     * OS trust store by default (an invalid cert fails the send). */
    req = WinHttpOpenRequest(con, L"POST", wpath, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) goto done;
    if (phase_ms > 0)
        WinHttpSetTimeouts(req, phase_ms, phase_ms, phase_ms, phase_ms);

    WinHttpAddRequestHeaders(req, L"Content-Type: application/json", (DWORD)-1L,
                             WINHTTP_ADDREQ_FLAG_ADD);
    /* The only encoding we ever set is gzip (the send path), so a non-NULL value
     * means gzip - avoids a UTF-8 -> wide conversion for a fixed header. */
    if (content_encoding && content_encoding[0])
        WinHttpAddRequestHeaders(req, L"Content-Encoding: gzip", (DWORD)-1L,
                                 WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body,
                            (DWORD)body_len, (DWORD)body_len, 0))
        goto done;
    if (deadline) {
        int remain = tls_remaining_ms(deadline);
        if (remain <= 0) goto done;
        WinHttpSetTimeouts(req, remain, remain, remain, remain);
    }
    if (!WinHttpReceiveResponse(req, NULL)) goto done;
    if (deadline && tls_remaining_ms(deadline) <= 0) goto done;

    if (WinHttpQueryHeaders(req,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                            WINHTTP_NO_HEADER_INDEX))
        rc = (int)status;

    /* Retry-After (if present) - the raw header value, parsed by the limiter.
     * WinHTTP returns it as a string; a small buffer suffices (seconds or a
     * ~29-char HTTP-date). */
    if (retry_after && retry_after_cap > 0) {
        wchar_t wra[64];
        DWORD wsz = sizeof(wra);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RETRY_AFTER,
                                WINHTTP_HEADER_NAME_BY_INDEX, wra, &wsz,
                                WINHTTP_NO_HEADER_INDEX))
            WideCharToMultiByte(CP_UTF8, 0, wra, -1, retry_after,
                                (int)retry_after_cap, NULL, NULL);
    }

    if (out && out_cap > 1) {
        DWORD total = 0, avail = 0, got = 0;
        for (;;) {
            if (deadline) {
                int remain = tls_remaining_ms(deadline);
                if (remain <= 0) {
                    rc = -1;
                    break;
                }
                WinHttpSetTimeouts(req, remain, remain, remain, remain);
            }
            if (!WinHttpQueryDataAvailable(req, &avail)) {
                rc = -1;
                break;
            }
            if (avail == 0) break;
            {
                DWORD room = (DWORD)(out_cap - 1 - total);
                DWORD toread;
                if (avail > room) {
                    if (require_full_body) {
                        rc = -2; /* PH_HTTP_RESPONSE_TOO_LARGE */
                        break;
                    }
                    toread = room; /* send path needs only a quota-notice prefix */
                } else {
                    toread = avail;
                }
                if (toread == 0) break;
                if (deadline) {
                    int remain = tls_remaining_ms(deadline);
                    if (remain <= 0) {
                        rc = -1;
                        break;
                    }
                    WinHttpSetTimeouts(req, remain, remain, remain, remain);
                }
                if (!WinHttpReadData(req, out + total, toread, &got) || got == 0) {
                    rc = -1;
                    break;
                }
                total += got;
                if (!require_full_body && total == out_cap - 1) break;
            }
        }
        out[total] = '\0';
    }

done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return rc;
}

int ph_tls_send(const char *host, int port, const char *path, const char *body,
                size_t body_len, int timeout_ms, const char *content_encoding,
                char *retry_after, size_t retry_after_cap, char *body_out,
                size_t body_out_cap) {
    /* Route the body prefix through do_tls's response-body reader (the same one
     * /flags/ uses), just into a small caller buffer. */
    return do_tls(host, port, path, body, body_len, timeout_ms, body_out,
                  body_out_cap, content_encoding, retry_after, retry_after_cap, 0);
}

int ph_tls_fetch(const char *host, int port, const char *path, const char *body,
                 size_t body_len, int timeout_ms, char *out, size_t out_cap) {
    return do_tls(host, port, path, body, body_len, timeout_ms, out, out_cap, NULL,
                  NULL, 0, 1);
}

#elif defined(__APPLE__) || defined(__linux__)

/*
 * POSIX HTTPS: we own the TCP socket and the HTTP/1.1 framing; the platform's
 * system TLS library owns the handshake and certificate verification. macOS
 * uses Secure Transport, Linux links the system OpenSSL. Both verify the cert
 * chain and match it to the host against the OS trust store, so a bad or
 * mismatched certificate fails the handshake. The socket, request builder, and
 * response parser below are shared; only the TLS read/write differs per backend.
 */
#include "ph_http.h"     /* ph__http_parse_response_meta / decode + PH_HTTP_RESPONSE_* */
#include "ph_internal.h" /* ph_send_meta, PH_HOST_CAP */
#include "ph_str.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

int ph_tls_available(void) { return 1; }

typedef struct {
    int fd;
    uint64_t deadline;
} tls_conn;

/* Bound each socket read/write by the remaining request budget. */
static void tls_sock_timeout(const tls_conn *tc) {
    int ms = tls_remaining_ms(tc->deadline);
    struct timeval tv;
    if (ms < 0) return; /* no deadline */
    if (ms == 0) ms = 1;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(tc->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(tc->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* Connect a TCP socket to host:port within the deadline, or -1. */
static int tls_connect(const char *host, int port, uint64_t deadline) {
    char portstr[16];
    struct addrinfo hints, *res = NULL, *ai;
    int fd = -1;
    tls_conn tc;
    snprintf(portstr, sizeof portstr, "%d", port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        tc.fd = fd;
        tc.deadline = deadline;
        tls_sock_timeout(&tc);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Build the raw HTTP/1.1 request (mirrors ph_http_build_request; Connection:
 * close so the server closes after the response and we read to EOF). */
static void tls_build_request(ph_strbuf *out, const char *host, int port,
                              const char *path, const char *body, size_t body_len,
                              const char *content_encoding) {
    char line[PH_HOST_CAP + 64];
    snprintf(line, sizeof line, "POST %s HTTP/1.1\r\n", (path && path[0]) ? path : "/");
    ph_strbuf_append_cstr(out, line);
    if (port == 443)
        snprintf(line, sizeof line, "Host: %s\r\n", host);
    else
        snprintf(line, sizeof line, "Host: %s:%d\r\n", host, port);
    ph_strbuf_append_cstr(out, line);
    ph_strbuf_append_cstr(out, "Content-Type: application/json\r\n");
    ph_strbuf_append_cstr(out, "User-Agent: posthog-c/" PH_VERSION_STRING "\r\n");
    if (content_encoding && content_encoding[0]) {
        snprintf(line, sizeof line, "Content-Encoding: %s\r\n", content_encoding);
        ph_strbuf_append_cstr(out, line);
    }
    snprintf(line, sizeof line, "Content-Length: %lu\r\n", (unsigned long)body_len);
    ph_strbuf_append_cstr(out, line);
    ph_strbuf_append_cstr(out, "Connection: close\r\n\r\n");
    ph_strbuf_append(out, body, body_len);
}

/* Turn a raw HTTP response into a status code, copying either the fully decoded
 * body (fetch) or the Retry-After + body prefix (send) into the caller's
 * buffers. Shared by both TLS backends. */
static int tls_finish(const char *resp, size_t resp_len, int require_full_body,
                      char *out, size_t out_cap, char *retry_after,
                      size_t retry_after_cap) {
    if (require_full_body)
        return ph__http_decode_response_body(resp, resp_len, out, out_cap);
    {
        ph_send_meta meta;
        int status;
        memset(&meta, 0, sizeof meta);
        status = ph__http_parse_response_meta(resp, resp_len, &meta);
        if (retry_after && retry_after_cap) {
            strncpy(retry_after, meta.retry_after, retry_after_cap - 1);
            retry_after[retry_after_cap - 1] = '\0';
        }
        if (out && out_cap) {
            strncpy(out, meta.body, out_cap - 1);
            out[out_cap - 1] = '\0';
        }
        return status;
    }
}

#if defined(__APPLE__)

/*
 * macOS/iOS: Secure Transport over the shared BSD socket. The cert chain is
 * matched to the host by SSLSetPeerDomainName (no BreakOnServerAuth override).
 * Secure Transport is deprecated in favour of Network.framework, but it is the
 * smallest synchronous C API for a sender thread; the warnings are suppressed.
 */
#include <CoreFoundation/CoreFoundation.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <Security/SecureTransport.h>

/* Secure Transport IO callbacks. A short read/write (timeout) returns
 * errSSLWouldBlock so the caller re-checks the deadline and retries. */
static OSStatus tls_read_cb(SSLConnectionRef c, void *data, size_t *len) {
    const tls_conn *tc = (const tls_conn *)c;
    size_t want = *len, got = 0;
    tls_sock_timeout(tc);
    while (got < want) {
        ssize_t n = read(tc->fd, (char *)data + got, want - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n == 0) {
            *len = got;
            return errSSLClosedGraceful;
        }
        if (errno == EINTR) continue;
        *len = got;
        return errSSLWouldBlock;
    }
    *len = got;
    return noErr;
}

static OSStatus tls_write_cb(SSLConnectionRef c, const void *data, size_t *len) {
    const tls_conn *tc = (const tls_conn *)c;
    size_t want = *len, sent = 0;
    tls_sock_timeout(tc);
    while (sent < want) {
        ssize_t n = write(tc->fd, (const char *)data + sent, want - sent);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        *len = sent;
        return errSSLWouldBlock;
    }
    *len = want;
    return noErr;
}

static int do_tls(const char *host, int port, const char *path, const char *body,
                  size_t body_len, int timeout_ms, char *out, size_t out_cap,
                  const char *content_encoding, char *retry_after,
                  size_t retry_after_cap, int require_full_body) {
    uint64_t deadline = tls_deadline(timeout_ms);
    tls_conn tc;
    SSLContextRef ctx = NULL;
    OSStatus st;
    ph_strbuf req;
    char *resp = NULL;
    size_t resp_len = 0, resp_cap;
    size_t sent = 0;
    int status = -1;

    if (out && out_cap) out[0] = '\0';
    if (retry_after && retry_after_cap) retry_after[0] = '\0';

    tc.fd = tls_connect(host, port, deadline);
    if (tc.fd < 0) return -1;
    tc.deadline = deadline;

    ctx = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
    if (!ctx) goto done;
    if (SSLSetIOFuncs(ctx, tls_read_cb, tls_write_cb) != noErr) goto done;
    if (SSLSetConnection(ctx, &tc) != noErr) goto done;
    if (SSLSetPeerDomainName(ctx, host, strlen(host)) != noErr) goto done;

    do {
        st = SSLHandshake(ctx);
    } while (st == errSSLWouldBlock && (!deadline || tls_remaining_ms(deadline) > 0));
    if (st != noErr) goto done; /* handshake or certificate verification failed */

    ph_strbuf_init(&req);
    tls_build_request(&req, host, port, path, body, body_len, content_encoding);
    if (req.oom) {
        ph_strbuf_free(&req);
        goto done;
    }
    {
        int send_ok = 1;
        while (sent < req.len) {
            size_t processed = 0;
            st = SSLWrite(ctx, req.data + sent, req.len - sent, &processed);
            sent += processed;
            if (st == errSSLWouldBlock) {
                if (deadline && tls_remaining_ms(deadline) <= 0) {
                    send_ok = 0;
                    break;
                }
                continue;
            }
            if (st != noErr) {
                send_ok = 0;
                break;
            }
        }
        ph_strbuf_free(&req);
        if (!send_ok) goto done;
    }

    resp_cap = require_full_body ? out_cap + 32768 : 8192;
    resp = (char *)malloc(resp_cap + 1);
    if (!resp) goto done;
    for (;;) {
        size_t got = 0, room = resp_cap - resp_len;
        if (room == 0) break;
        if (deadline && tls_remaining_ms(deadline) <= 0) break;
        st = SSLRead(ctx, resp + resp_len, room, &got);
        resp_len += got;
        resp[resp_len] = '\0';
        if (st == errSSLClosedGraceful || st == errSSLClosedNoNotify ||
            st == errSSLClosedAbort)
            break;
        if (st == errSSLWouldBlock) continue;
        if (st != noErr) break;
    }

    status = tls_finish(resp, resp_len, require_full_body, out, out_cap,
                        retry_after, retry_after_cap);

done:
    if (ctx) {
        SSLClose(ctx);
        CFRelease(ctx);
    }
    if (tc.fd >= 0) close(tc.fd);
    free(resp);
    return status;
}

#pragma clang diagnostic pop

#else /* __linux__: system OpenSSL (links libssl/libcrypto) */

#include <openssl/ssl.h>
#include <openssl/x509v3.h>

static int do_tls(const char *host, int port, const char *path, const char *body,
                  size_t body_len, int timeout_ms, char *out, size_t out_cap,
                  const char *content_encoding, char *retry_after,
                  size_t retry_after_cap, int require_full_body) {
    uint64_t deadline = tls_deadline(timeout_ms);
    tls_conn tc;
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    ph_strbuf req;
    char *resp = NULL;
    size_t resp_len = 0, resp_cap;
    size_t sent = 0;
    int status = -1;

    if (out && out_cap) out[0] = '\0';
    if (retry_after && retry_after_cap) retry_after[0] = '\0';

    tc.fd = tls_connect(host, port, deadline);
    if (tc.fd < 0) return -1;
    tc.deadline = deadline;

    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) goto done;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    /* Verify the chain against the system trust store; combined with SSL_set1_host
     * below, a bad chain or a hostname mismatch then fails SSL_connect. */
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) goto done;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    ssl = SSL_new(ctx);
    if (!ssl) goto done;
    SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (SSL_set1_host(ssl, host) != 1) goto done;        /* hostname to verify */
    if (SSL_set_tlsext_host_name(ssl, host) != 1) goto done; /* SNI */
    if (SSL_set_fd(ssl, tc.fd) != 1) goto done;

    for (;;) {
        int rc;
        tls_sock_timeout(&tc);
        rc = SSL_connect(ssl);
        if (rc == 1) break; /* handshake + certificate + hostname verified */
        if (SSL_get_error(ssl, rc) == SSL_ERROR_SYSCALL && errno == EINTR &&
            (!deadline || tls_remaining_ms(deadline) > 0))
            continue;
        goto done; /* handshake, certificate, or hostname verification failed */
    }

    ph_strbuf_init(&req);
    tls_build_request(&req, host, port, path, body, body_len, content_encoding);
    if (req.oom) {
        ph_strbuf_free(&req);
        goto done;
    }
    {
        int send_ok = 1;
        tls_sock_timeout(&tc);
        while (sent < req.len) {
            size_t left = req.len - sent;
            int chunk = left > INT_MAX ? INT_MAX : (int)left;
            int n = SSL_write(ssl, req.data + sent, chunk);
            if (n > 0) {
                sent += (size_t)n;
                continue;
            }
            if (SSL_get_error(ssl, n) == SSL_ERROR_SYSCALL && errno == EINTR &&
                (!deadline || tls_remaining_ms(deadline) > 0)) {
                tls_sock_timeout(&tc);
                continue;
            }
            send_ok = 0;
            break;
        }
        ph_strbuf_free(&req);
        if (!send_ok) goto done;
    }

    resp_cap = require_full_body ? out_cap + 32768 : 8192;
    resp = (char *)malloc(resp_cap + 1);
    if (!resp) goto done;
    for (;;) {
        size_t room = resp_cap - resp_len;
        int n;
        if (room == 0) break;
        if (deadline && tls_remaining_ms(deadline) <= 0) break;
        tls_sock_timeout(&tc);
        n = SSL_read(ssl, resp + resp_len, room > INT_MAX ? INT_MAX : (int)room);
        if (n > 0) {
            resp_len += (size_t)n;
            resp[resp_len] = '\0';
            continue;
        }
        if (SSL_get_error(ssl, n) == SSL_ERROR_SYSCALL && errno == EINTR) continue;
        break; /* clean close (ZERO_RETURN), timeout, or error - stop reading */
    }

    status = tls_finish(resp, resp_len, require_full_body, out, out_cap,
                        retry_after, retry_after_cap);

done:
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if (ctx) SSL_CTX_free(ctx);
    if (tc.fd >= 0) close(tc.fd);
    free(resp);
    return status;
}

#endif /* backend select */

int ph_tls_send(const char *host, int port, const char *path, const char *body,
                size_t body_len, int timeout_ms, const char *content_encoding,
                char *retry_after, size_t retry_after_cap, char *body_out,
                size_t body_out_cap) {
    return do_tls(host, port, path, body, body_len, timeout_ms, body_out,
                  body_out_cap, content_encoding, retry_after, retry_after_cap, 0);
}

int ph_tls_fetch(const char *host, int port, const char *path, const char *body,
                 size_t body_len, int timeout_ms, char *out, size_t out_cap) {
    return do_tls(host, port, path, body, body_len, timeout_ms, out, out_cap, NULL,
                  NULL, 0, 1);
}

#else /* other platforms: no system TLS backend wired up yet */

int ph_tls_available(void) { return 0; }

int ph_tls_send(const char *host, int port, const char *path, const char *body,
                size_t body_len, int timeout_ms, const char *content_encoding,
                char *retry_after, size_t retry_after_cap, char *body_out,
                size_t body_out_cap) {
    (void)host;
    (void)port;
    (void)path;
    (void)body;
    (void)body_len;
    (void)timeout_ms;
    (void)content_encoding;
    if (retry_after && retry_after_cap) retry_after[0] = '\0';
    if (body_out && body_out_cap) body_out[0] = '\0';
    return -1;
}

int ph_tls_fetch(const char *host, int port, const char *path, const char *body,
                 size_t body_len, int timeout_ms, char *out, size_t out_cap) {
    (void)host;
    (void)port;
    (void)path;
    (void)body;
    (void)body_len;
    (void)timeout_ms;
    if (out && out_cap) out[0] = '\0';
    return -1;
}

#endif
