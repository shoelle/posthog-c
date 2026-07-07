/*
 * Error tracking: ph_capture_exception emits the full $exception_list payload
 * (type, value, mechanism, raw stack frames), and the sender's scrub preserves
 * that payload while still stripping denylisted scalar props.
 */
#include "posthog.h"
#include "ph_internal.h"
#include "mock_transport.h"
#include "test_util.h"

#include <stdio.h>
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

static void remove_key(ph_props *props, const char *key) {
    int i, k;
    for (i = 0; i < props->count;) {
        if (strcmp(props->items[i].key, key) == 0) {
            for (k = i; k + 1 < props->count; k++) props->items[k] = props->items[k + 1];
            props->count--;
        } else {
            i++;
        }
    }
}

static int redact_exception_message(const char *event, ph_props *props, void *user) {
    (void)user;
    if (strcmp(event, "$exception") == 0) {
        remove_key(props, "$exception_message");
        ph_props_set_str(props, "$exception_message", "redacted");
    }
    return 1;
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

    /* --- before_send can redact raw exception text before $exception_list is built --- */
    {
        ph_config cfg;
        ph_stackframe frame;
        ph_exception ex;
        const char *b;
        static const char *deny[] = {"filename"};
        init_sdk(&cfg);
        cfg.before_send = redact_exception_message;
        cfg.property_denylist = deny;
        cfg.property_denylist_count = 1;
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();

        memset(&frame, 0, sizeof(frame));
        frame.function = "step";
        frame.filename = "secret.cpp";
        frame.in_app = 1;

        memset(&ex, 0, sizeof(ex));
        ex.type = "NativeAssertion";
        ex.message = "secret message";
        ex.handled = 1;
        ex.frames = &frame;
        ex.frame_count = 1;
        ph_capture_exception(&ex);
        ph_flush(2000);

        b = mock_batch(0);
        CHECK_CONTAINS(b, "\"value\":\"redacted\"");
        CHECK_NOT_CONTAINS(b, "secret message");
        CHECK_NOT_CONTAINS(b, "secret.cpp");
        CHECK_NOT_CONTAINS(b, "\"filename\"");
        ph_shutdown();
    }

    /* --- oversized frame lists are capped before entering the fixed event blob --- */
    {
        ph_config cfg;
        ph_stackframe frames[PH_MAX_EXCEPTION_FRAMES + 2];
        char names[PH_MAX_EXCEPTION_FRAMES + 2][16];
        ph_exception ex;
        int i;
        init_sdk(&cfg);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();

        memset(frames, 0, sizeof(frames));
        for (i = 0; i < PH_MAX_EXCEPTION_FRAMES + 2; i++) {
            snprintf(names[i], sizeof(names[i]), "frame_%d", i);
            frames[i].function = names[i];
            frames[i].in_app = 1;
        }

        memset(&ex, 0, sizeof(ex));
        ex.type = "Overflow";
        ex.message = "many frames";
        ex.handled = 1;
        ex.frames = frames;
        ex.frame_count = PH_MAX_EXCEPTION_FRAMES + 2;
        ph_capture_exception(&ex);
        ph_flush(2000);

        {
            char last_kept[16], first_dropped[16];
            snprintf(last_kept, sizeof(last_kept), "frame_%d", PH_MAX_EXCEPTION_FRAMES - 1);
            snprintf(first_dropped, sizeof(first_dropped), "frame_%d", PH_MAX_EXCEPTION_FRAMES);
            CHECK_CONTAINS(mock_batch(0), "frame_0");
            CHECK_CONTAINS(mock_batch(0), last_kept);
            CHECK_NOT_CONTAINS(mock_batch(0), first_dropped);
        }
        ph_shutdown();
    }

    /* --- super props on exceptions are still denylist-scrubbed --- */
    {
        ph_config cfg;
        ph_props sp;
        ph_exception ex;
        static const char *deny[] = {"secret_super"};
        init_sdk(&cfg);
        cfg.property_denylist = deny;
        cfg.property_denylist_count = 1;
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();

        ph_props_init(&sp);
        ph_props_set_str(&sp, "secret_super", "leak");
        ph_props_set_str(&sp, "keep_super", "ok");
        ph_register(&sp);

        memset(&ex, 0, sizeof(ex));
        ex.type = "E";
        ex.message = "m";
        ex.handled = 1;
        ph_capture_exception(&ex);
        ph_flush(2000);

        CHECK_CONTAINS(mock_batch(0), "\"keep_super\":\"ok\"");
        CHECK_NOT_CONTAINS(mock_batch(0), "secret_super");
        CHECK_NOT_CONTAINS(mock_batch(0), "leak");
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
