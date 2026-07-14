/*
 * End-to-end tests through the public API with the mock transport installed,
 * so they assert the real capture -> queue -> sender -> batch path (and its
 * identity / super-property / group behaviour) without touching the network.
 */
#include "posthog.h"
#include "ph_thread.h"
#include "mock_transport.h"
#include "test_util.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

/* Init with auto-flush disabled so the test controls exactly when a batch is
 * sent, via ph_flush(). */
static void init_test_sdk(void) {
    ph_config cfg;
    ph_config_defaults(&cfg);
    cfg.api_key = "phc_capture";
    cfg.api_host = "http://127.0.0.1:9/ingest"; /* never hit: mock replaces transport */
    cfg.distinct_id = "anon-123";
    cfg.flush_at = 100000;       /* don't flush on count */
    cfg.flush_interval_ms = 60000; /* don't flush on time */
    cfg.preload_flags = 0;         /* no /flags/ round-trip in tests */
    cfg.enabled = 1;
    CHECK(ph_init(&cfg) == PH_OK);
    mock_reset();
    mock_install();
}

/* Captures the latest on_stats JSON for the stats test below. */
static char g_stats_json[512];
static int g_stats_calls;
static ph_mutex g_stats_lock;
static void stats_cb(const char *json, size_t len, void *user) {
    size_t n = len;
    (void)user;
    ph_mutex_lock(&g_stats_lock);
    g_stats_calls++;
    if (n >= sizeof(g_stats_json)) n = sizeof(g_stats_json) - 1;
    memcpy(g_stats_json, json, n);
    g_stats_json[n] = '\0';
    ph_mutex_unlock(&g_stats_lock);
}

static void reentrant_stats_cb(const char *json, size_t len, void *user) {
    (void)json;
    (void)len;
    (void)user;
    ph_flush(-1); /* sender-thread calls are guarded no-ops, not deadlocks */
    ph_shutdown();
    ph_mutex_lock(&g_stats_lock);
    g_stats_calls++;
    ph_mutex_unlock(&g_stats_lock);
}

static int reentrant_scrub_cb(const char *event, ph_props *props, void *user) {
    (void)event;
    (void)props;
    (void)user;
    ph_flush(-1);
    ph_shutdown();
    return 1;
}

static int count_text(const char *hay, const char *needle) {
    int n = 0;
    size_t len = strlen(needle);
    while (hay && (hay = strstr(hay, needle)) != NULL) {
        n++;
        hay += len;
    }
    return n;
}

/* Concatenate every delivered batch body into `out`. */
static void join_batches(char *out, size_t cap) {
    int i, n = mock_batch_count();
    size_t used = 0;
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        const char *b = mock_batch(i);
        size_t bl;
        if (!b) continue;
        bl = strlen(b);
        if (used + bl + 1 >= cap) break;
        memcpy(out + used, b, bl);
        used += bl;
    }
    out[used] = '\0';
}

/* ph_identify()/ph_group() wake the sender for a flag refresh, so the events
 * under test can legitimately span more than one POST, and a loaded CI runner
 * can starve the sender past a single flush deadline. Flush until the last
 * captured event has actually been delivered (FIFO: the last event landing means
 * every earlier one did too), then join every batch so callers assert on the
 * aggregate instead of assuming a single batch arrived in time. */
static void deliver_and_join(char *out, size_t cap, const char *last_event) {
    int i;
    for (i = 0; i < 10; i++) {
        ph_flush(2000);
        join_batches(out, cap);
        if (strstr(out, last_event)) return;
        sleep_ms(25);
    }
}

void suite_capture(void) {
    /* --- privacy-sensitive additions remain opt-in --- */
    {
        ph_config cfg;
        ph_config_defaults(&cfg);
        CHECK(cfg.disable_geoip == 0);
    }

    /* --- basic capture roundtrip, anonymous by default --- */
    {
        ph_props p;
        init_test_sdk();
        ph_props_init(&p);
        ph_props_set_str(&p, "panel", "code");
        ph_capture("editor_panel_opened", &p);
        ph_capture("ping", NULL);
        ph_flush(2000);
        CHECK(mock_batch_count() == 1); /* both events, one POST */
        CHECK_CONTAINS(mock_batch(0), "\"event\":\"editor_panel_opened\"");
        CHECK_CONTAINS(mock_batch(0), "\"event\":\"ping\"");
        CHECK_CONTAINS(mock_batch(0), "\"distinct_id\":\"anon-123\"");
        CHECK_CONTAINS(mock_batch(0), "\"panel\":\"code\"");
        CHECK_CONTAINS(mock_batch(0), "\"api_key\":\"phc_capture\"");
        CHECK_CONTAINS(mock_batch(0), "\"$process_person_profile\":false");
        ph_shutdown();
    }

    /* --- sub-threshold events wait for flush/interval instead of sending one-by-one --- */
    {
        init_test_sdk();
        ph_capture("wait_for_batch", NULL);
        sleep_ms(150);
        CHECK(mock_batch_count() == 0);
        ph_flush(2000);
        CHECK(mock_batch_count() == 1);
        CHECK_CONTAINS(mock_batch(0), "\"event\":\"wait_for_batch\"");
        ph_shutdown();
    }

    /* --- identify switches the id and drops anonymity for later events --- */
    {
        ph_props sp;
        init_test_sdk();
        ph_props_init(&sp);
        ph_props_set_str(&sp, "plan", "pro");
        ph_identify("acct-777", &sp);
        ph_capture("after_identify", NULL);
        {
            char joined[16384];
            deliver_and_join(joined, sizeof(joined), "\"event\":\"after_identify\"");
            CHECK_CONTAINS(joined, "\"event\":\"$identify\"");
            CHECK_CONTAINS(joined, "\"$set\":{");
            CHECK_CONTAINS(joined, "\"plan\":\"pro\"");
            CHECK_CONTAINS(joined, "\"distinct_id\":\"acct-777\"");
            CHECK_CONTAINS(joined, "\"event\":\"after_identify\"");
            CHECK_NOT_CONTAINS(joined, "\"$process_person_profile\":false");
        }
        ph_shutdown();
    }

    /* --- PH_NEVER also suppresses profiles on identity/control events --- */
    {
        ph_config cfg;
        char joined[16384];
        ph_config_defaults(&cfg);
        cfg.api_key = "phc_never";
        cfg.api_host = "http://127.0.0.1:9/ingest";
        cfg.distinct_id = "anon-never";
        cfg.flush_at = 100000;
        cfg.flush_interval_ms = 60000;
        cfg.preload_flags = 0;
        cfg.person_profiles = PH_NEVER;
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();

        ph_identify("acct-never", NULL);
        ph_alias("alias-never", "acct-never");
        ph_group("company", "never-co", NULL);
        ph_capture("after_never_controls", NULL);
        deliver_and_join(joined, sizeof(joined), "\"event\":\"after_never_controls\"");

        CHECK_CONTAINS(joined, "\"event\":\"$identify\"");
        CHECK_CONTAINS(joined, "\"event\":\"$create_alias\"");
        CHECK_CONTAINS(joined, "\"event\":\"$groupidentify\"");
        CHECK_CONTAINS(joined, "\"event\":\"after_never_controls\"");
        CHECK(count_text(joined, "\"$process_person_profile\":false") == 4);
        ph_shutdown();
    }

    /* --- super properties merge into every event --- */
    {
        ph_props sp;
        init_test_sdk();
        ph_props_init(&sp);
        ph_props_set_str(&sp, "build", "1.2.3");
        ph_register(&sp);
        ph_capture("something", NULL);
        ph_flush(2000);
        CHECK_CONTAINS(mock_batch(0), "\"build\":\"1.2.3\"");
        ph_shutdown();
    }

    /* --- group scoping stamps $groups on later events --- */
    {
        init_test_sdk();
        ph_group("game", "asteroids", NULL);
        ph_capture("played", NULL);
        ph_flush(2000);
        CHECK(mock_batch_count() >= 1);
        CHECK_CONTAINS(mock_batch(0), "\"$groupidentify\"");
        CHECK_CONTAINS(mock_batch(0), "\"$groups\":{");
        CHECK_CONTAINS(mock_batch(0), "\"game\":\"asteroids\"");
        ph_shutdown();
    }

    /* --- a failing transport doesn't crash; the batch was attempted --- */
    {
        init_test_sdk();
        mock_set_status(500);
        ph_capture("will_fail", NULL);
        /* 3 retries with exponential backoff (~700ms) before the batch settles;
         * a generous flush budget so a slow CI runner doesn't race the window
         * (ph_flush returns as soon as the sender is idle, so this is free). */
        ph_flush(10000);
        CHECK(mock_batch_count() == 4); /* initial try + default 3 retries */
        ph_shutdown();
    }

    /* --- a client error (4xx) is deterministic and is NOT retried --- */
    {
        init_test_sdk();
        mock_set_status(400);
        ph_capture("bad_request", NULL);
        ph_flush(2000);
        CHECK(mock_batch_count() == 1); /* one attempt only */
        ph_shutdown();
    }

    /* --- disabled SDK is a no-op and safe --- */
    {
        ph_config cfg;
        ph_config_defaults(&cfg);
        cfg.api_key = "phc_x";
        cfg.enabled = 0;
        CHECK(ph_init(&cfg) == PH_OK);
        ph_capture("ignored", NULL); /* no-op */
        ph_flush(10);
        ph_shutdown();
    }

    /* --- enabled init rejects missing/malformed/over-cap configuration --- */
    {
        ph_config cfg;
        char long_id[PH_DISTINCT_ID_CAP + 1];
        memset(long_id, 'x', sizeof(long_id) - 1);
        long_id[sizeof(long_id) - 1] = '\0';

        ph_config_defaults(&cfg);
        CHECK(ph_init(&cfg) == PH_ERR_BADARG); /* api_key is required */
        cfg.api_key = "phc_cfg";
        cfg.api_host = "ftp://invalid";
        CHECK(ph_init(&cfg) == PH_ERR_BADARG);
        cfg.api_host = "https://us.i.posthog.com";
        CHECK(ph_init(&cfg) == PH_ERR_BADARG); /* stable distinct_id is required */
        cfg.distinct_id = long_id;
        CHECK(ph_init(&cfg) == PH_ERR_BADARG);
        cfg.distinct_id = "valid";
        cfg.property_denylist_count = 1;
        cfg.property_denylist = NULL;
        CHECK(ph_init(&cfg) == PH_ERR_BADARG);
        cfg.property_denylist_count = 0;
        cfg.max_batch = 10001;
        CHECK(ph_init(&cfg) == PH_ERR_BADARG);
        cfg.max_batch = 50;
        cfg.max_queue = 100001;
        CHECK(ph_init(&cfg) == PH_ERR_BADARG);
        cfg.max_queue = 1000;
        cfg.max_batch_bytes = -1;
        CHECK(ph_init(&cfg) == PH_ERR_BADARG);
    }

    /* --- current identity is observable so reset ids can be persisted --- */
    {
        char id[PH_DISTINCT_ID_CAP];
        char tiny[4];
        init_test_sdk();
        CHECK(ph_get_distinct_id(id, sizeof(id)) == PH_OK);
        CHECK(strcmp(id, "anon-123") == 0);
        CHECK(ph_get_distinct_id(tiny, sizeof(tiny)) == PH_ERR_TRUNCATED);
        ph_identify("account-42", NULL);
        CHECK(ph_get_distinct_id(id, sizeof(id)) == PH_OK);
        CHECK(strcmp(id, "account-42") == 0);
        ph_reset();
        CHECK(ph_get_distinct_id(id, sizeof(id)) == PH_OK);
        CHECK(id[0] != '\0');
        CHECK(strcmp(id, "account-42") != 0);
        CHECK(strcmp(id, "anon-123") != 0);
        ph_shutdown();
        CHECK(ph_get_distinct_id(id, sizeof(id)) == PH_ERR_DISABLED);
    }

    /* --- ph_capture returns the fate of the event --- */
    {
        init_test_sdk();
        CHECK(ph_capture("ok", NULL) == PH_OK);
        CHECK(ph_capture(NULL, NULL) == PH_ERR_BADARG);
        CHECK(ph_capture("", NULL) == PH_ERR_BADARG);
        {
            char fit_name[PH_EVENT_NAME_CAP];
            char long_name[PH_EVENT_NAME_CAP + 1];
            char fit_json[PH_EVENT_NAME_CAP + 16];
            char long_json[PH_EVENT_NAME_CAP + 16];
            memset(fit_name, 'f', sizeof(fit_name) - 1);
            fit_name[sizeof(fit_name) - 1] = '\0';
            memset(long_name, 't', sizeof(long_name) - 1);
            long_name[sizeof(long_name) - 1] = '\0';
            snprintf(fit_json, sizeof(fit_json), "\"event\":\"%s\"", fit_name);
            snprintf(long_json, sizeof(long_json),
                     "\"event\":\"%.*s\"", PH_EVENT_NAME_CAP - 1, long_name);

            CHECK(ph_capture(fit_name, NULL) == PH_OK);
            CHECK(ph_capture(long_name, NULL) == PH_ERR_TRUNCATED);
            ph_flush(2000);
            CHECK_CONTAINS(mock_batch(0), fit_json);
            CHECK_CONTAINS(mock_batch(0), long_json);
        }
        ph_shutdown();
        CHECK(ph_capture("after_shutdown", NULL) == PH_ERR_DISABLED); /* SDK off */
    }

    /* --- identify uses the same capped ID for its event and later captures --- */
    {
        char long_id[PH_DISTINCT_ID_CAP + 40];
        char capped[PH_DISTINCT_ID_CAP];
        char needle[PH_DISTINCT_ID_CAP + 32];
        memset(long_id, 'i', sizeof(long_id) - 1);
        long_id[sizeof(long_id) - 1] = '\0';
        memset(capped, 'i', sizeof(capped) - 1);
        capped[sizeof(capped) - 1] = '\0';
        snprintf(needle, sizeof(needle), "\"distinct_id\":\"%s\"", capped);

        init_test_sdk();
        ph_identify(long_id, NULL);
        ph_capture("after_long_identify", NULL);
        ph_flush(2000);
        CHECK(count_text(mock_batch(0), needle) == 2);
        ph_shutdown();
    }

    /* --- a rate-limited capture reports PH_ERR_RATE_LIMITED --- */
    {
        ph_config cfg;
        int i, limited = 0;
        ph_config_defaults(&cfg);
        cfg.api_key = "phc_rl";
        cfg.distinct_id = "anon-rl";
        cfg.flush_at = 100000;
        cfg.flush_interval_ms = 60000;
        cfg.preload_flags = 0;
        cfg.rate_limit_per_sec = 1; /* burst 1: one token, then empty */
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        CHECK(ph_capture("first", NULL) == PH_OK); /* spends the token */
        for (i = 0; i < 5; i++)
            if (ph_capture("flood", NULL) == PH_ERR_RATE_LIMITED) limited = 1;
        CHECK(limited);
        ph_flush(2000);
        ph_shutdown();
    }

    /* --- max_batch_bytes splits an over-cap batch into multiple POSTs --- */
    {
        ph_config cfg;
        int i, batches;
        ph_config_defaults(&cfg);
        cfg.api_key = "phc_bytes";
        cfg.distinct_id = "anon-bytes";
        cfg.flush_at = 100000;
        cfg.flush_interval_ms = 60000;
        cfg.preload_flags = 0;
        cfg.max_batch = 100;       /* count alone would keep these in one POST */
        cfg.max_batch_bytes = 400; /* tiny: the serialized events blow past it */
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        for (i = 0; i < 8; i++) ph_capture("byte_cap_event", NULL);
        ph_flush(2000);
        batches = mock_batch_count();
        CHECK(batches > 1); /* the byte cap forced a split */
        for (i = 0; i < batches; i++)
            CHECK(strlen(mock_batch(i)) <= (size_t)cfg.max_batch_bytes);
        ph_shutdown();
    }

    /* --- an unsplittable event over max_batch_bytes is never POSTed --- */
    {
        ph_config cfg;
        ph_config_defaults(&cfg);
        cfg.api_key = "phc_single_bytes";
        cfg.distinct_id = "anon-single-bytes";
        cfg.flush_at = 100000;
        cfg.flush_interval_ms = 60000;
        cfg.preload_flags = 0;
        cfg.max_batch_bytes = 100; /* smaller than one serialized bare event */
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        ph_capture("single_over_cap", NULL);
        ph_flush(2000);
        CHECK(mock_batch_count() == 0);
        ph_shutdown();
    }

    /* --- a split batch stops POSTing as soon as server backpressure arms --- */
    {
        ph_config cfg;
        int i;
        ph_config_defaults(&cfg);
        cfg.api_key = "phc_split_hold";
        cfg.distinct_id = "anon-split-hold";
        cfg.flush_at = 100000;
        cfg.flush_interval_ms = 60000;
        cfg.preload_flags = 0;
        cfg.max_batch = 100;
        cfg.max_batch_bytes = 400;
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        mock_set_status(429);
        mock_set_retry_after("60");
        for (i = 0; i < 8; i++) ph_capture("split_hold_event", NULL);
        ph_flush(2000);
        CHECK(mock_batch_count() == 1);
        ph_shutdown();
    }

    /* --- on_stats emits a periodic snapshot with a drop breakdown --- */
    {
        ph_config cfg;
        int i;
        int stats_calls;
        char stats_json[sizeof(g_stats_json)];
        ph_config_defaults(&cfg);
        cfg.api_key = "phc_stats";
        cfg.distinct_id = "anon-stats";
        cfg.flush_at = 100000;
        cfg.flush_interval_ms = 60000;
        cfg.preload_flags = 0;
        cfg.rate_limit_per_sec = 1; /* generate some rate-limit drops */
        cfg.stats_interval_ms = 30; /* emit quickly for the test */
        cfg.on_stats = stats_cb;
        ph_mutex_init(&g_stats_lock);
        ph_mutex_lock(&g_stats_lock);
        g_stats_calls = 0;
        g_stats_json[0] = '\0';
        ph_mutex_unlock(&g_stats_lock);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        mock_set_status(400); /* accepted event fails; drops remain limiter-only */
        for (i = 0; i < 6; i++) ph_capture("flood", NULL); /* 1 sent, ~5 dropped */
        sleep_ms(200);                                     /* let stats fire */
        ph_mutex_lock(&g_stats_lock);
        stats_calls = g_stats_calls;
        memcpy(stats_json, g_stats_json, sizeof(stats_json));
        ph_mutex_unlock(&g_stats_lock);
        CHECK(stats_calls > 0);
        CHECK_CONTAINS(stats_json, "\"queued\":");
        CHECK_CONTAINS(stats_json, "\"failed\":1");
        CHECK_CONTAINS(stats_json,
                       "\"dropped\":{\"total\":5,\"rate_limited\":5");
        ph_shutdown();
        ph_mutex_destroy(&g_stats_lock);
    }

    /* --- sender-thread callbacks cannot self-deadlock lifecycle/draining --- */
    {
        ph_config cfg;
        int calls;
        ph_config_defaults(&cfg);
        cfg.api_key = "phc_stats_reentrant";
        cfg.distinct_id = "anon-stats-reentrant";
        cfg.flush_at = 100000;
        cfg.flush_interval_ms = 20;
        cfg.preload_flags = 0;
        cfg.stats_interval_ms = 10;
        cfg.on_stats = reentrant_stats_cb;
        ph_mutex_init(&g_stats_lock);
        ph_mutex_lock(&g_stats_lock);
        g_stats_calls = 0;
        ph_mutex_unlock(&g_stats_lock);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        ph_capture("callback_guard", NULL);
        sleep_ms(100);
        ph_mutex_lock(&g_stats_lock);
        calls = g_stats_calls;
        ph_mutex_unlock(&g_stats_lock);
        CHECK(calls > 0);
        CHECK(ph_capture("still_running", NULL) == PH_OK);
        ph_shutdown();
        ph_mutex_destroy(&g_stats_lock);
    }

    /* --- caller-thread exception scrubbers cannot tear down their own call --- */
    {
        ph_config cfg;
        ph_exception ex;
        ph_config_defaults(&cfg);
        cfg.api_key = "phc_scrub_reentrant";
        cfg.distinct_id = "anon-scrub-reentrant";
        cfg.flush_at = 100000;
        cfg.flush_interval_ms = 60000;
        cfg.preload_flags = 0;
        cfg.before_send = reentrant_scrub_cb;
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        memset(&ex, 0, sizeof(ex));
        ex.type = "Guarded";
        ex.message = "still alive";
        ex.handled = 1;
        ph_capture_exception(&ex);
        CHECK(ph_capture("after_reentrant_scrub", NULL) == PH_OK);
        ph_flush(2000);
        CHECK_CONTAINS(mock_batch(0), "after_reentrant_scrub");
        ph_shutdown();
    }
}
