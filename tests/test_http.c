#include "ph_http.h"
#include "ph_str.h"
#include "test_util.h"

#include <string.h>

void suite_http(void) {
    ph_strbuf out;
    ph_result r;
    const char *body = "{\"x\":1}"; /* 7 bytes */

    ph_strbuf_init(&out);
    r = ph_http_build_request("http://localhost:8000/ingest/batch/", body,
                              strlen(body), NULL, &out);
    CHECK(r == PH_OK);
    CHECK_CONTAINS(out.data, "POST /ingest/batch/ HTTP/1.1\r\n");
    CHECK_CONTAINS(out.data, "Host: localhost:8000\r\n");
    CHECK_CONTAINS(out.data, "Content-Type: application/json\r\n");
    CHECK_CONTAINS(out.data, "Content-Length: 7\r\n");
    CHECK_CONTAINS(out.data, "\r\n\r\n{\"x\":1}");
    CHECK_NOT_CONTAINS(out.data, "Content-Encoding"); /* none when NULL */
    ph_strbuf_free(&out);

    /* content_encoding adds the header */
    ph_strbuf_init(&out);
    r = ph_http_build_request("http://localhost:8000/batch/", body, strlen(body),
                              "gzip", &out);
    CHECK(r == PH_OK);
    CHECK_CONTAINS(out.data, "Content-Encoding: gzip\r\n");
    ph_strbuf_free(&out);

    /* default port 80 is omitted from the Host header */
    ph_strbuf_init(&out);
    r = ph_http_build_request("http://example.com/batch/", body, strlen(body), NULL, &out);
    CHECK(r == PH_OK);
    CHECK_CONTAINS(out.data, "Host: example.com\r\n");
    CHECK_NOT_CONTAINS(out.data, "example.com:");
    ph_strbuf_free(&out);

    /* The plaintext request builder is http-only; https is delivered via the
     * TLS backend (ph_tls / WinHTTP), not this path. */
    ph_strbuf_init(&out);
    r = ph_http_build_request("https://us.i.posthog.com/batch/", body, strlen(body), NULL, &out);
    CHECK(r == PH_ERR);
    ph_strbuf_free(&out);

    /* unsupported scheme */
    ph_strbuf_init(&out);
    r = ph_http_build_request("ftp://nope/", body, strlen(body), NULL, &out);
    CHECK(r == PH_ERR);
    ph_strbuf_free(&out);
}
