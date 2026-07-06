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
    CHECK(mock_batch_count() == 1); /* one failed attempt */
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

    ph_shutdown();
    remove(path);
}
