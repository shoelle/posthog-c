#include "ph_tls.h"
#include "ph_time.h"
#include "posthog.h"

#include <limits.h>

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

static uint64_t tls_deadline(int timeout_ms) {
    uint64_t now, add;
    if (timeout_ms <= 0) return 0;
    now = ph_now_mono_ns();
    add = (uint64_t)timeout_ms * 1000000ull;
    return UINT64_MAX - now < add ? UINT64_MAX : now + add;
}

static int tls_remaining_ms(uint64_t deadline) {
    uint64_t now, left, ms;
    if (!deadline) return -1;
    now = ph_now_mono_ns();
    if (now >= deadline) return 0;
    left = deadline - now;
    ms = (left + 999999ull) / 1000000ull;
    return ms > (uint64_t)INT_MAX ? INT_MAX : (int)ms;
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

#else /* non-Windows: TLS backend not yet vendored (v0.2 is Windows-first) */

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
