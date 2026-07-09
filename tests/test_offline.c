/*
 * Offline spill + replay: when a send fails and offline_path is set, the batch
 * is written to disk and re-sent on the next drain once the transport recovers.
 */
#include "posthog.h"
#include "ph_internal.h" /* PH_OFFLINE_FILENAME */
#include "mock_transport.h"
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void temp_dir(char *out, size_t cap) {
    const char *t = getenv("TEMP");
    if (!t) t = getenv("TMPDIR");
    if (!t) t = ".";
    snprintf(out, cap, "%s/ph_offline_test", t);
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

/* Write exactly the given bytes (no trailing newline added) - used to forge a
 * torn spill file, as a process killed mid-write would leave behind. */
static void write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, strlen(data), f);
    fclose(f);
}

static int any_batch_contains(const char *needle) {
    int i, n = mock_batch_count();
    for (i = 0; i < n; i++) {
        const char *b = mock_batch(i);
        if (b && strstr(b, needle)) return 1;
    }
    return 0;
}

void suite_offline(void) {
    char dir[256], path[300];
    ph_config cfg;
    char *content;
    size_t clen;

    temp_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s", dir, PH_OFFLINE_FILENAME);
    remove(path); /* clean any leftover from a prior run */

    ph_config_defaults(&cfg);
    cfg.api_key = "phc_offline";
    cfg.api_host = "http://127.0.0.1:9/ingest";
    cfg.distinct_id = "anon-o";
    cfg.flush_at = 100000;
    cfg.flush_interval_ms = 60000;
    cfg.preload_flags = 0;
    cfg.enabled = 1;
    cfg.offline_path = dir;
    CHECK(ph_init(&cfg) == PH_OK);
    mock_reset();
    mock_install();

    /* --- offline: the send fails, so the batch spills to disk --- */
    mock_set_status(500);
    ph_capture("offline_ev", NULL);
    ph_flush(2000);
    CHECK(mock_batch_count() == 4); /* initial failed attempt + default 3 retries */
    content = read_file(path, &clen);
    CHECK_MSG(content != NULL && clen > 0, "expected a non-empty spill file");
    if (content) {
        CHECK_CONTAINS(content, "offline_ev");
        CHECK_CONTAINS(content, "phc_offline");
        free(content);
    }

    /* --- reconnect: the next drain replays the spilled batch --- */
    mock_reset(); /* clears bodies; status back to 200 */
    ph_flush(2000);
    CHECK(mock_batch_count() >= 1);
    CHECK_CONTAINS(mock_batch(0), "offline_ev");
    content = read_file(path, &clen);
    CHECK_MSG(content == NULL || clen == 0, "expected the spill file drained");
    if (content) free(content);

    /* --- torn spill line: a process killed mid-spill leaves a partial batch
     * with no trailing newline. offline_spill always newline-terminates a
     * record, so replay must drop that torn tail rather than POST it forever
     * (a 400 loop that would poison the queue on every run). --- */

    /* (a) torn-only file: nothing valid to send; the file must be discarded. */
    mock_reset();
    write_file(path, "{\"batch\":\"torn_only\""); /* note: no trailing '\n' */
    ph_flush(2000);
    CHECK_MSG(!any_batch_contains("torn_only"),
              "a torn (unterminated) spill line must not be POSTed");
    content = read_file(path, &clen);
    CHECK_MSG(content == NULL || clen == 0,
              "a torn-only spill file must be discarded, not retained");
    if (content) free(content);

    /* (b) one complete record then a torn tail: the complete line replays, the
     * torn tail is dropped, and the file drains clean. */
    mock_reset();
    write_file(path, "{\"batch\":\"good_line\"}\n{\"batch\":\"torn_tail\"");
    ph_flush(2000);
    CHECK_MSG(any_batch_contains("good_line"),
              "the complete record before a torn tail must still replay");
    CHECK_MSG(!any_batch_contains("torn_tail"),
              "the torn tail after a complete record must be dropped");
    content = read_file(path, &clen);
    CHECK_MSG(content == NULL || clen == 0,
              "spill file must drain once the torn tail is dropped");
    if (content) free(content);

    /* --- permanent client error: a 4xx other than 429 is a deterministic
     * reject. A live batch that gets a 400 is dropped, not spilled - spilling
     * would just re-POST and fail on every drain, blocking the queue. --- */
    mock_reset();
    mock_set_status(400);
    ph_capture("client_err_ev", NULL);
    ph_flush(2000);
    CHECK_MSG(mock_batch_count() == 1, "a 4xx must not be retried");
    content = read_file(path, &clen);
    CHECK_MSG(content == NULL || clen == 0, "a 4xx batch must be dropped, not spilled");
    if (content) free(content);

    /* --- 4xx on replay: each rejected record is dropped and replay continues
     * to the next, rather than keeping the poison line and stopping (which
     * blocked everything queued behind it). --- */
    mock_reset();
    mock_set_status(400);
    write_file(path, "{\"batch\":\"poison_a\"}\n{\"batch\":\"poison_b\"}\n");
    ph_flush(2000);
    CHECK_MSG(any_batch_contains("poison_a") && any_batch_contains("poison_b"),
              "replay must continue past a 4xx-rejected record, not stop on it");
    content = read_file(path, &clen);
    CHECK_MSG(content == NULL || clen == 0,
              "4xx-rejected spill records must be dropped, not retained");
    if (content) free(content);

    ph_shutdown();
    remove(path);
}
