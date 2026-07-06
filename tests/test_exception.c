/*
 * Error tracking: ph_capture_exception emits the full $exception_list payload
 * (type, value, mechanism, raw stack frames), and the sender's scrub preserves
 * that payload while still stripping denylisted scalar props.
 */
#include "posthog.h"
#include "mock_transport.h"
#include "test_util.h"

#include <string.h>

static void init_sdk(ph_config *cfg) {
    ph_config_defaults(cfg);
    cfg->api_key = "phc_exc";
    cfg->api_host = "http://127.0.0.1:9/x";
    cfg->distinct_id = "anon-e";
    cfg->flush_at = 100000;
    cfg->flush_interval_ms = 60000;
    cfg->preload_flags = 0;
    cfg->enabled = 1;
}

void suite_exception(void) {
    /* --- full payload with stack frames --- */
    {
        ph_config cfg;
        ph_stackframe frames[2];
        ph_exception ex;
        const char *b;
        init_sdk(&cfg);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();

        memset(frames, 0, sizeof(frames));
        frames[0].function = "sim::step";
        frames[0].filename = "sim.cpp";
        frames[0].module = "engine";
        frames[0].lineno = 412;
        frames[0].in_app = 1;
        frames[1].function = "main";
        frames[1].filename = "main.cpp";
        frames[1].lineno = 20;
        frames[1].in_app = 1;

        memset(&ex, 0, sizeof(ex));
        ex.type = "NativeAssertion";
        ex.message = "entity index out of range";
        ex.handled = 1;
        ex.synthetic = 0;
        ex.frames = frames;
        ex.frame_count = 2;

        ph_capture_exception(&ex);
        ph_flush(2000);

        b = mock_batch(0);
        CHECK(mock_batch_count() == 1);
        CHECK_CONTAINS(b, "\"event\":\"$exception\"");
        CHECK_CONTAINS(b, "\"$exception_list\":[{");
        CHECK_CONTAINS(b, "\"type\":\"NativeAssertion\"");
        CHECK_CONTAINS(b, "\"value\":\"entity index out of range\"");
        CHECK_CONTAINS(b, "\"mechanism\":{\"handled\":true,\"synthetic\":false}");
        CHECK_CONTAINS(b, "\"stacktrace\":{\"type\":\"raw\",\"frames\":[");
        CHECK_CONTAINS(b, "\"function\":\"sim::step\"");
        CHECK_CONTAINS(b, "\"filename\":\"sim.cpp\"");
        CHECK_CONTAINS(b, "\"module\":\"engine\"");
        CHECK_CONTAINS(b, "\"lineno\":412");
        CHECK_CONTAINS(b, "\"function\":\"main\"");
        CHECK_CONTAINS(b, "\"$exception_level\":\"warning\""); /* handled => warning */
        ph_shutdown();
    }

    /* --- unhandled => level error --- */
    {
        ph_config cfg;
        ph_exception ex;
        init_sdk(&cfg);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();

        memset(&ex, 0, sizeof(ex));
        ex.type = "Segfault";
        ex.message = "boom";
        ex.handled = 0;
        ph_capture_exception(&ex);
        ph_flush(2000);

        CHECK_CONTAINS(mock_batch(0), "\"$exception_level\":\"error\"");
        CHECK_CONTAINS(mock_batch(0), "\"mechanism\":{\"handled\":false");
        ph_shutdown();
    }

    /* --- scrub keeps $exception_list but strips a denylisted extra prop --- */
    {
        ph_config cfg;
        ph_props extra;
        ph_exception ex;
        const char *b;
        static const char *deny[] = {"user_email"};
        init_sdk(&cfg);
        cfg.property_denylist = deny;
        cfg.property_denylist_count = 1;
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();

        ph_props_init(&extra);
        ph_props_set_str(&extra, "user_email", "leak@x.com");
        ph_props_set_str(&extra, "screen", "level3");

        memset(&ex, 0, sizeof(ex));
        ex.type = "E";
        ex.message = "m";
        ex.handled = 1;
        ex.extra = &extra;
        ph_capture_exception(&ex);
        ph_flush(2000);

        b = mock_batch(0);
        CHECK_CONTAINS(b, "\"$exception_list\":[{"); /* payload survived the scrub */
        CHECK_CONTAINS(b, "\"screen\":\"level3\"");  /* kept extra */
        CHECK_NOT_CONTAINS(b, "leak@x.com");          /* denylisted extra stripped */
        CHECK_NOT_CONTAINS(b, "\"user_email\"");
        ph_shutdown();
    }
}
