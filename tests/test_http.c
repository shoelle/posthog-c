#include "ph_http.h"
#include "ph_str.h"
#include "test_util.h"

#include <string.h>

static void test_request_builder(void) {
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

static void test_send_response_meta(void) {
    ph_send_meta meta;
    const char *plain =
        "HTTP/1.1 429 Too Many Requests\r\n"
        "Retry-After: 2\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    const char *chunked =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "11\r\n"
        "{\"quota_limited\":"
        "\r\n"
        "B\r\n"
        "[\"events\"]}"
        "\r\n"
        "0\r\n"
        "\r\n";
    const char *partial_chunked =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "11\r\n"
        "{\"quota_limited\":"
        "\r\n";
    const char *close_delimited =
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "{";

    memset(&meta, 0, sizeof meta);
    CHECK(ph__http_parse_response_meta(plain, strlen(plain), &meta) == 429);
    CHECK(strcmp(meta.retry_after, "2") == 0);
    CHECK(strcmp(meta.body, "") == 0);
    CHECK(ph__http_send_response_complete(plain, strlen(plain)));

    memset(&meta, 0, sizeof meta);
    CHECK(ph__http_parse_response_meta(chunked, strlen(chunked), &meta) == 200);
    CHECK(strcmp(meta.body, "{\"quota_limited\":[\"events\"]}") == 0);
    CHECK_NOT_CONTAINS(meta.body, "\r\nB\r\n");
    CHECK(ph__http_send_response_complete(chunked, strlen(chunked)));

    CHECK(!ph__http_send_response_complete(partial_chunked,
                                           strlen(partial_chunked)));
    CHECK(ph__http_send_response_complete(close_delimited,
                                          strlen(close_delimited)));
}

void suite_http(void) {
    test_request_builder();
    test_send_response_meta();
}
