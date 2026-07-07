/*
 * End-to-end tests through the public API with the mock transport installed,
 * so they assert the real capture -> queue -> sender -> batch path (and its
 * identity / super-property / group behaviour) without touching the network.
 */
#include "posthog.h"
#include "mock_transport.h"
#include "test_util.h"

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

void suite_capture(void) {
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
        ph_flush(2000);
        CHECK(mock_batch_count() == 1);
        CHECK_CONTAINS(mock_batch(0), "\"event\":\"$identify\"");
        CHECK_CONTAINS(mock_batch(0), "\"$set\":{");
        CHECK_CONTAINS(mock_batch(0), "\"plan\":\"pro\"");
        CHECK_CONTAINS(mock_batch(0), "\"distinct_id\":\"acct-777\"");
        CHECK_CONTAINS(mock_batch(0), "\"event\":\"after_identify\"");
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
        ph_flush(2000);
        CHECK(mock_batch_count() == 4); /* initial try + default 3 retries */
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
}
