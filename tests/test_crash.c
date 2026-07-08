/*
 * signal_crash (v0.6): the crash-record codec, signal-name mapping, and the
 * replay path that turns a persisted crash into a $exception. The async-signal
 * handler itself (an actual fault) is exercised out-of-process by an
 * integration test; here we forge the record a handler would have written and
 * assert replay ships the right $exception through the normal path.
 */
#include "posthog.h"
#include "ph_crash.h"
#include "mock_transport.h"
#include "test_util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

static void temp_dir(char *out, size_t cap) {
    const char *t = getenv("TEMP");
    if (!t) t = getenv("TMPDIR");
    if (!t) t = ".";
    snprintf(out, cap, "%s/ph_crash_test", t);
}

static void write_bytes(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, len, f);
    fclose(f);
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

void suite_crash(void) {
    ph_crash_info a, b;
    char rec[PH_CRASH_RECORD_MAX];
    char bad[PH_CRASH_RECORD_MAX];
    size_t n;
    unsigned seh = 0xC0000005u;
    char dir[256], recpath[320];
    ph_config cfg;
    char *content;
    size_t clen;

    /* --- codec: a record round-trips exactly (module table + frames) --- */
    memset(&a, 0, sizeof a);
    a.sig = 11;
    a.fault_addr = 0xdeadbeefULL;
    a.module_count = 2;
    strcpy(a.modules[0], "app.exe");
    strcpy(a.modules[1], "engine.dll");
    a.frame_count = 3;
    a.frame_module[0] = 0;
    a.frame_off[0] = 0x1234;
    a.frame_module[1] = 1;
    a.frame_off[1] = 0x40;
    a.frame_module[2] = -1; /* unknown module -> offset is the absolute address */
    a.frame_off[2] = 0x7fff0000ULL;
    n = ph_crash_encode(&a, rec, sizeof rec);
    CHECK(n > 0);
    memset(&b, 0xAA, sizeof b);
    CHECK(ph_crash_decode(rec, n, &b) == 1);
    CHECK(b.sig == 11);
    CHECK(b.fault_addr == 0xdeadbeefULL);
    CHECK(b.module_count == 2);
    CHECK(strcmp(b.modules[0], "app.exe") == 0);
    CHECK(strcmp(b.modules[1], "engine.dll") == 0);
    CHECK(b.frame_count == 3);
    CHECK(b.frame_module[0] == 0 && b.frame_off[0] == 0x1234);
    CHECK(b.frame_module[1] == 1 && b.frame_off[1] == 0x40);
    CHECK(b.frame_module[2] == -1 && b.frame_off[2] == 0x7fff0000ULL);

    /* --- codec: garbage is rejected, not misread --- */
    CHECK(ph_crash_decode("nope", 4, &b) == 0); /* bad magic */
    CHECK(ph_crash_decode(rec, 3, &b) == 0);     /* truncated header */
    memcpy(bad, rec, n);
    bad[4] = 9; /* version low byte -> unknown version */
    CHECK(ph_crash_decode(bad, n, &b) == 0);
    memcpy(bad, rec, n);
    bad[18] = (char)0xFF; /* module_count -> 0xFFFF, impossibly large */
    bad[19] = (char)0xFF;
    CHECK(ph_crash_decode(bad, n, &b) == 0);

    /* --- signal names: POSIX, SEH, and the fallback --- */
    CHECK(strcmp(ph_crash_signal_name(SIGSEGV), "SIGSEGV") == 0);
    CHECK(strcmp(ph_crash_signal_name((int)seh), "EXCEPTION_ACCESS_VIOLATION") == 0);
    CHECK(strcmp(ph_crash_signal_name(1234567), "SIGNAL") == 0);

    /* --- replay: a forged record ships a $exception through the normal path --- */
    temp_dir(dir, sizeof dir);
    MKDIR(dir); /* ok if it already exists */
    snprintf(recpath, sizeof recpath, "%s/%s", dir, PH_CRASH_FILENAME);
    remove(recpath);

    memset(&a, 0, sizeof a);
    a.sig = SIGSEGV;
    a.fault_addr = 0;
    a.module_count = 1;
    strcpy(a.modules[0], "crash_demo.exe");
    a.frame_count = 2;
    a.frame_module[0] = 0;
    a.frame_off[0] = 0x1361;
    a.frame_module[1] = 0;
    a.frame_off[1] = 0x78d;
    n = ph_crash_encode(&a, rec, sizeof rec);
    write_bytes(recpath, rec, n);

    ph_config_defaults(&cfg);
    cfg.api_key = "phc_crash";
    cfg.api_host = "http://127.0.0.1:9/ingest";
    cfg.distinct_id = "anon-c";
    cfg.flush_at = 100000;
    cfg.flush_interval_ms = 60000;
    cfg.preload_flags = 0;
    cfg.enabled = 1;
    cfg.offline_path = dir;
    cfg.crash_handler = 0; /* replay by hand so init doesn't touch the network */
    CHECK(ph_init(&cfg) == PH_OK);
    mock_reset();
    mock_install();

    CHECK(ph_signal_crash_replay(dir) == 1);
    ph_flush(2000);
    CHECK(mock_batch_count() >= 1);
    if (mock_batch_count() >= 1) {
        const char *body = mock_batch(0);
        CHECK_CONTAINS(body, "$exception");
        CHECK_CONTAINS(body, "SIGSEGV");           /* mapped signal name */
        CHECK_CONTAINS(body, "\"handled\":false"); /* an uncaught crash */
        CHECK_CONTAINS(body, "\"type\":\"raw\"");  /* the raw stacktrace */
        CHECK_CONTAINS(body, "crash_demo.exe");    /* module basename ... */
        CHECK_CONTAINS(body, "0x1361");            /* ... + module-relative offset */
        CHECK_CONTAINS(body, "crash_origin");      /* origin tag ... */
        CHECK_CONTAINS(body, "signal_crash");      /* ... = signal_crash */
    }

    /* the record is consumed exactly once */
    content = read_file(recpath, &clen);
    CHECK_MSG(content == NULL || clen == 0, "crash record must be removed after replay");
    if (content) free(content);
    CHECK(ph_signal_crash_replay(dir) == 0);

    /* a corrupt record is discarded, never replayed forever */
    mock_reset();
    write_bytes(recpath, "garbage-not-a-record", 20);
    CHECK(ph_signal_crash_replay(dir) == 0);
    CHECK(mock_batch_count() == 0);
    content = read_file(recpath, &clen);
    CHECK_MSG(content == NULL || clen == 0, "corrupt crash record must be discarded");
    if (content) free(content);

    ph_shutdown();
    remove(recpath);
}
