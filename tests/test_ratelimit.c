/*
 * Server-directed backpressure: the sender honors HTTP 429 / Retry-After and
 * holds sends until the window clears, instead of draining batch after batch
 * into a throttled endpoint. Covers the pure parser + state machine and the
 * end-to-end gate through the sender and mock transport.
 */
#include "posthog.h"
#include "ph_internal.h" /* PH_OFFLINE_FILENAME */
#include "ph_ratelimit.h"
#include "ph_thread.h" /* ph_sleep_ms */
#include "mock_transport.h"
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEC_NS(s) ((uint64_t)(s) * 1000000000ull)

static void temp_dir(char *out, size_t cap, const char *name) {
    const char *t = getenv("TEMP");
    if (!t) t = getenv("TMPDIR");
    if (!t) t = ".";
    snprintf(out, cap, "%s/%s", t, name);
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    size_t rd;
    if (out_len) *out_len = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

/* --- Retry-After parsing (pure) --------------------------------------- */
static void test_parse(void) {
    /* delay-seconds */
    CHECK(ph_ratelimit_parse_retry_after("120", 0) == 120000);
    CHECK(ph_ratelimit_parse_retry_after("0", 0) == 0);
    CHECK(ph_ratelimit_parse_retry_after("  5  ", 0) == 5000); /* leading space */
    /* absurd values clamp to the ceiling rather than overflowing */
    CHECK(ph_ratelimit_parse_retry_after("999999999", 0) == PH_RL_MAX_BACKOFF_MS);
    /* absent / malformed */
    CHECK(ph_ratelimit_parse_retry_after(NULL, 0) == -1);
    CHECK(ph_ratelimit_parse_retry_after("", 0) == -1);
    CHECK(ph_ratelimit_parse_retry_after("soon", 0) == -1);
    CHECK(ph_ratelimit_parse_retry_after("5x", 0) == -1);

    /* IMF-fixdate, anchored at the Unix epoch so the arithmetic is checkable */
    CHECK(ph_ratelimit_parse_retry_after("Thu, 01 Jan 1970 00:00:10 GMT", 0) == 10000);
    CHECK(ph_ratelimit_parse_retry_after("Thu, 01 Jan 1970 01:00:00 GMT", 0) == 3600000);
    /* the day-name is optional */
    CHECK(ph_ratelimit_parse_retry_after("01 Jan 1970 00:00:10 GMT", 0) == 10000);
    /* a date already in the past yields 0, never a negative delay */
    CHECK(ph_ratelimit_parse_retry_after("Thu, 01 Jan 1970 00:00:10 GMT", SEC_NS(20)) == 0);
    /* a bad month is unparseable */
    CHECK(ph_ratelimit_parse_retry_after("Mon, 01 Xxx 1970 00:00:10 GMT", 0) == -1);
}

/* --- state machine (now_mono = 0 => deterministic, zero jitter) ------- */
static void test_state(void) {
    ph_ratelimit rl;

    ph_ratelimit_init(&rl);
    CHECK(!ph_ratelimit_blocked(&rl, 0));

    /* 200 never engages the limiter */
    CHECK(ph_ratelimit_note_response(&rl, 200, NULL, 0, 0) == 0);
    CHECK(!ph_ratelimit_blocked(&rl, 0));

    /* 429 + Retry-After: exactly 10s (jitter is 0 when now_mono is 0) */
    CHECK(ph_ratelimit_note_response(&rl, 429, "10", 0, 0) == 1);
    CHECK(ph_ratelimit_blocked(&rl, 0));
    CHECK(ph_ratelimit_blocked(&rl, SEC_NS(9)));
    CHECK(!ph_ratelimit_blocked(&rl, SEC_NS(10)));
    CHECK(ph_ratelimit_remaining_ms(&rl, 0) == 10000);
    CHECK(ph_ratelimit_remaining_ms(&rl, SEC_NS(5)) == 5000);

    /* bare 429 falls back to the default window */
    ph_ratelimit_init(&rl);
    CHECK(ph_ratelimit_note_response(&rl, 429, NULL, 0, 0) == 1);
    CHECK(ph_ratelimit_remaining_ms(&rl, 0) == PH_RL_DEFAULT_BACKOFF_MS);

    /* 503 without Retry-After does NOT engage (ordinary 5xx retry handles it) */
    ph_ratelimit_init(&rl);
    CHECK(ph_ratelimit_note_response(&rl, 503, NULL, 0, 0) == 0);
    CHECK(!ph_ratelimit_blocked(&rl, 0));
    /* 503 carrying a Retry-After does engage */
    CHECK(ph_ratelimit_note_response(&rl, 503, "5", 0, 0) == 1);
    CHECK(ph_ratelimit_remaining_ms(&rl, 0) == 5000);
}

/* --- end-to-end: the sender holds while throttled, resumes after ------ */
static void test_sender_gate(void) {
    ph_config cfg;
    ph_config_defaults(&cfg);
    cfg.api_key = "phc_rl";
    cfg.api_host = "http://127.0.0.1:9";
    cfg.distinct_id = "anon-rl";
    cfg.flush_at = 100000;      /* never auto-flush; we drive it explicitly */
    cfg.flush_interval_ms = 60000;
    cfg.preload_flags = 0;
    cfg.enabled = 1;
    CHECK(ph_init(&cfg) == PH_OK);
    mock_reset();
    mock_install();

    /* Throttle with a 1s window. One batch pays the 429; then the gate holds. */
    mock_set_status(429);
    mock_set_retry_after("1");
    ph_capture("evt_a", NULL);
    ph_flush(2000);
    CHECK(mock_batch_count() == 1); /* exactly one attempt, no hammering */

    /* Server is healthy again, but we are still inside the window: the queued
     * event must not go out yet. */
    mock_set_status(200);
    ph_capture("evt_b", NULL);
    ph_flush(300); /* shorter than the ~1s hold */
    CHECK(mock_batch_count() == 1); /* held */

    /* Once the window clears, the backlog flushes. */
    ph_sleep_ms(1500);
    ph_capture("evt_c", NULL);
    ph_flush(2000);
    CHECK(mock_batch_count() >= 2); /* resumed */
    CHECK_CONTAINS(mock_batch(mock_batch_count() - 1), "evt_c");

    ph_shutdown();
}

/* --- PostHog quota notice: a 2xx body pauses the sender like a 429 ---- */
static void test_quota_gate(void) {
    ph_config cfg;
    ph_config_defaults(&cfg);
    cfg.api_key = "phc_q";
    cfg.api_host = "http://127.0.0.1:9";
    cfg.distinct_id = "anon-q";
    cfg.flush_at = 100000;
    cfg.flush_interval_ms = 60000;
    cfg.preload_flags = 0;
    cfg.enabled = 1;
    CHECK(ph_init(&cfg) == PH_OK);
    mock_reset();
    mock_install();

    /* A plain 200 with no quota notice does not pause: both batches go out. */
    mock_set_status(200);
    mock_set_send_body("{\"status\":1}");
    ph_capture("evt_1", NULL);
    ph_flush(2000);
    CHECK(mock_batch_count() == 1);
    ph_capture("evt_2", NULL);
    ph_flush(2000);
    CHECK(mock_batch_count() == 2); /* not held */

    /* Now the server reports events quota-limited on a 200. That batch is still
     * accepted (200), but the sender arms the hold for what follows. */
    mock_set_send_body("{\"status\":1,\"quota_limited\":[\"events\"]}");
    ph_capture("evt_3", NULL);
    ph_flush(2000);
    CHECK(mock_batch_count() == 3);

    /* Even with the server healthy again, the next event is held by the window. */
    mock_set_send_body("{\"status\":1}");
    ph_capture("evt_4", NULL);
    ph_flush(300);
    CHECK(mock_batch_count() == 3); /* held */

    ph_shutdown();
}

/* --- offline replay: quota-limited 2xx keeps the spill line ---------- */
static void test_quota_keeps_offline_replay(void) {
    char dir[256], path[300];
    char *content;
    size_t clen;
    ph_config cfg;

    temp_dir(dir, sizeof(dir), "ph_rl_quota_replay");
    snprintf(path, sizeof(path), "%s/%s", dir, PH_OFFLINE_FILENAME);
    remove(path);

    ph_config_defaults(&cfg);
    cfg.api_key = "phc_qr";
    cfg.api_host = "http://127.0.0.1:9";
    cfg.distinct_id = "anon-qr";
    cfg.flush_at = 100000;
    cfg.flush_interval_ms = 60000;
    cfg.preload_flags = 0;
    cfg.enabled = 1;
    cfg.offline_path = dir;
    CHECK(ph_init(&cfg) == PH_OK);
    mock_reset();
    mock_install();

    mock_set_status(500);
    ph_capture("offline_quota_ev", NULL);
    ph_flush(2000);

    mock_reset();
    mock_set_status(200);
    mock_set_send_body("{\"status\":1,\"quota_limited\":[\"events\"]}");
    ph_flush(2000);
    CHECK(mock_batch_count() == 1);

    content = read_file(path, &clen);
    CHECK_MSG(content != NULL && clen > 0, "expected quota-limited replay to stay on disk");
    if (content) {
        CHECK_CONTAINS(content, "offline_quota_ev");
        free(content);
    }

    ph_shutdown();
    remove(path);
}

/* --- shutdown during a hold persists queued events when offline_path exists --- */
static void test_shutdown_spills_held_queue(void) {
    char dir[256], path[300];
    char *content;
    size_t clen;
    ph_config cfg;

    temp_dir(dir, sizeof(dir), "ph_rl_shutdown_hold");
    snprintf(path, sizeof(path), "%s/%s", dir, PH_OFFLINE_FILENAME);
    remove(path);

    ph_config_defaults(&cfg);
    cfg.api_key = "phc_sd";
    cfg.api_host = "http://127.0.0.1:9";
    cfg.distinct_id = "anon-sd";
    cfg.flush_at = 100000;
    cfg.flush_interval_ms = 60000;
    cfg.preload_flags = 0;
    cfg.enabled = 1;
    cfg.offline_path = dir;
    CHECK(ph_init(&cfg) == PH_OK);
    mock_reset();
    mock_install();

    mock_set_status(429);
    mock_set_retry_after("2");
    ph_capture("trips_hold", NULL);
    ph_flush(2000);

    mock_reset();
    mock_set_status(200);
    ph_capture("held_at_shutdown", NULL);
    ph_shutdown();

    content = read_file(path, &clen);
    CHECK_MSG(content != NULL && clen > 0, "expected held queue to spill on shutdown");
    if (content) {
        CHECK_CONTAINS(content, "held_at_shutdown");
        free(content);
    }
    remove(path);
}

void suite_ratelimit(void) {
    test_parse();
    test_state();
    test_sender_gate();
    test_quota_gate();
    test_quota_keeps_offline_replay();
    test_shutdown_spills_held_queue();
}
