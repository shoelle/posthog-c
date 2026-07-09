/*
 * Fuzz target: the HTTP response parser used on every POST — status line,
 * headers (Retry-After), and chunked-body decoding. The other surface that
 * consumes server/MITM-controlled bytes. Feed the raw response to both the
 * meta parser and the "response complete?" check.
 */
#include "fuzz.h"
#include "ph_http.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ph_send_meta meta;
    memset(&meta, 0, sizeof meta);
    (void)ph__http_parse_response_meta((const char *)data, size, &meta);
    (void)ph__http_send_response_complete((const char *)data, size);
    return 0;
}

static fuzz_seed seeds[12];
static size_t g_count;
static int inited;

const fuzz_seed *fuzz_seeds(size_t *count) {
    if (!inited) {
        size_t i = 0;
        static const char s0[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
        static const char s1[] = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 120\r\n\r\n";
        static const char s2[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
        /* chunk size larger than the body — must not over-read */
        static const char s3[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nffffffffffffffff\r\nAAAA";
        static const char s4[] = "HTTP/1.1 200 OK\r\nContent-Length: 999999999999\r\n\r\n";
        /* chunked with no terminating 0-chunk */
        static const char s5[] = "HTTP/1.1 200 OK\r\ntransfer-encoding:  gzip, chunked \r\n\r\n3\r\nabc\r\n";
        static const char s6[] = "HTTP/1.1 200\r\n\r\n{\"quota_limited\":[\"events\"]}";
        static const char s7[] = "HTTP/"; /* truncated status line */
        static const char s8[] = "\r\n\r\n"; /* header terminator only */
        static const char s9[] = "HTTP/1.1 503 x\r\nRetry-After: Wed, 21 Oct 2099 07:28:00 GMT\r\n\r\n";
        static const char s10[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nab"; /* short final chunk */
        seeds[i].data = s0;  seeds[i++].len = sizeof s0 - 1;
        seeds[i].data = s1;  seeds[i++].len = sizeof s1 - 1;
        seeds[i].data = s2;  seeds[i++].len = sizeof s2 - 1;
        seeds[i].data = s3;  seeds[i++].len = sizeof s3 - 1;
        seeds[i].data = s4;  seeds[i++].len = sizeof s4 - 1;
        seeds[i].data = s5;  seeds[i++].len = sizeof s5 - 1;
        seeds[i].data = s6;  seeds[i++].len = sizeof s6 - 1;
        seeds[i].data = s7;  seeds[i++].len = sizeof s7 - 1;
        seeds[i].data = s8;  seeds[i++].len = sizeof s8 - 1;
        seeds[i].data = s9;  seeds[i++].len = sizeof s9 - 1;
        seeds[i].data = s10; seeds[i++].len = sizeof s10 - 1;
        g_count = i;
        inited = 1;
    }
    *count = g_count;
    return seeds;
}
