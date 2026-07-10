/*
 * Privacy + reliability: before_send scrubber, property_denylist, and the
 * capture-path rate limiter. All observed through the mock transport.
 */
#include "posthog.h"
#include "mock_transport.h"
#include "test_util.h"

#include <stdio.h>
#include <string.h>

static void base_cfg(ph_config *cfg) {
    ph_config_defaults(cfg);
    cfg->api_key = "phc_scrub";
    cfg->api_host = "http://127.0.0.1:9/ingest";
    cfg->distinct_id = "anon-s";
    cfg->flush_at = 100000;
    cfg->flush_interval_ms = 60000;
    cfg->preload_flags = 0;
    cfg->enabled = 1;
}

static void start(ph_config *cfg) {
    CHECK(ph_init(cfg) == PH_OK);
    mock_reset();
    mock_install();
}

static int count_occurrences(const char *hay, const char *needle) {
    int n = 0;
    const char *p = hay;
    size_t l = strlen(needle);
    if (!hay) return 0;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += l;
    }
    return n;
}

/* --- hooks --- */

static int drop_secret(const char *event, ph_props *props, void *user) {
    (void)props;
    (void)user;
    return strcmp(event, "secret_event") == 0 ? 0 : 1; /* 0 drops */
}

static int redact(const char *event, ph_props *props, void *user) {
    int i, k;
    (void)event;
    (void)user;
    for (i = 0; i < props->count;) {
        if (strcmp(props->items[i].key, "password") == 0) {
            for (k = i; k + 1 < props->count; k++) props->items[k] = props->items[k + 1];
            props->count--;
        } else {
            i++;
        }
    }
    ph_props_set_bool(props, "scrubbed", 1);
    return 1;
}

void suite_scrub(void) {
    /* --- before_send can drop an event entirely --- */
    {
        ph_config cfg;
        base_cfg(&cfg);
        cfg.before_send = drop_secret;
        start(&cfg);
        ph_capture("kept_event", NULL);
        ph_capture("secret_event", NULL);
        ph_flush(2000);
        CHECK(mock_batch_count() == 1);
        CHECK_CONTAINS(mock_batch(0), "kept_event");
        CHECK_NOT_CONTAINS(mock_batch(0), "secret_event");
        CHECK(ph_dropped_events() == 1);
        ph_shutdown();
    }

    /* --- before_send can redact / add properties --- */
    {
        ph_config cfg;
        ph_props p;
        base_cfg(&cfg);
        cfg.before_send = redact;
        start(&cfg);
        ph_props_init(&p);
        ph_props_set_str(&p, "password", "hunter2");
        ph_props_set_str(&p, "user", "bob");
        ph_capture("login", &p);
        ph_flush(2000);
        CHECK_CONTAINS(mock_batch(0), "\"user\":\"bob\"");
        CHECK_NOT_CONTAINS(mock_batch(0), "hunter2");
        CHECK_NOT_CONTAINS(mock_batch(0), "\"password\"");
        CHECK_CONTAINS(mock_batch(0), "\"scrubbed\":true");
        ph_shutdown();
    }

    /* --- property_denylist strips keys before send --- */
    {
        ph_config cfg;
        ph_props p;
        static const char *deny[] = {"secret", "token"};
        base_cfg(&cfg);
        cfg.property_denylist = deny;
        cfg.property_denylist_count = 2;
        start(&cfg);
        ph_props_init(&p);
        ph_props_set_str(&p, "secret", "xyz");
        ph_props_set_str(&p, "keep", "ok");
        ph_props_set_str(&p, "token", "abc");
        ph_capture("event", &p);
        ph_flush(2000);
        CHECK_CONTAINS(mock_batch(0), "\"keep\":\"ok\"");
        CHECK_NOT_CONTAINS(mock_batch(0), "\"secret\"");
        CHECK_NOT_CONTAINS(mock_batch(0), "\"token\"");
        CHECK_NOT_CONTAINS(mock_batch(0), "xyz");
        CHECK_NOT_CONTAINS(mock_batch(0), "abc");
        ph_shutdown();
    }

    /* --- identify/group caller properties use the same privacy pass --- */
    {
        ph_config cfg;
        ph_props identify, group;
        static const char *deny[] = {"secret", "token"};
        base_cfg(&cfg);
        cfg.property_denylist = deny;
        cfg.property_denylist_count = 2;
        start(&cfg);

        ph_props_init(&identify);
        ph_props_set_str(&identify, "secret", "identify-pii");
        ph_props_set_str(&identify, "plan", "pro");
        ph_identify("privacy-user", &identify);

        ph_props_init(&group);
        ph_props_set_str(&group, "token", "group-pii");
        ph_props_set_int(&group, "seats", 12);
        ph_group("company", "privacy-co", &group);

        ph_flush(2000);
        CHECK_CONTAINS(mock_batch(0), "\"$set\":{\"plan\":\"pro\"}");
        CHECK_CONTAINS(mock_batch(0), "\"$group_set\":{\"seats\":12}");
        CHECK_NOT_CONTAINS(mock_batch(0), "identify-pii");
        CHECK_NOT_CONTAINS(mock_batch(0), "group-pii");
        ph_shutdown();
    }

    /* --- identify/group props are also scrubbed on the /flags/ request body,
     *     not just the $identify/$groupidentify events. Regression: person_/
     *     group_properties used to be serialized raw, leaking denylisted and
     *     before_send-redacted values to /flags/ on every refetch. --- */
    {
        ph_config cfg;
        ph_props identify, group;
        static const char *deny[] = {"secret", "token"};
        base_cfg(&cfg);
        cfg.property_denylist = deny;
        cfg.property_denylist_count = 2;
        cfg.before_send = redact; /* also strips "password" and adds "scrubbed" */
        start(&cfg);

        ph_props_init(&identify);
        ph_props_set_str(&identify, "secret", "identify-pii");   /* denylist */
        ph_props_set_str(&identify, "password", "hunter2");      /* before_send */
        ph_props_set_str(&identify, "plan", "pro");              /* kept */
        ph_identify("privacy-user", &identify);

        ph_props_init(&group);
        ph_props_set_str(&group, "token", "group-pii");          /* denylist */
        ph_props_set_int(&group, "seats", 12);                   /* kept */
        ph_group("company", "privacy-co", &group);

        mock_set_flags_response("{\"flags\":{}}");
        ph_reload_feature_flags();
        CHECK_NOT_CONTAINS(mock_last_fetch_body(), "identify-pii");
        CHECK_NOT_CONTAINS(mock_last_fetch_body(), "hunter2");
        CHECK_NOT_CONTAINS(mock_last_fetch_body(), "group-pii");
        CHECK_CONTAINS(mock_last_fetch_body(), "\"plan\":\"pro\"");
        CHECK_CONTAINS(mock_last_fetch_body(), "\"seats\":12");
        ph_shutdown();
    }

    /* --- privacy does not let super props crowd out explicit event props --- */
    {
        ph_config cfg;
        ph_props super, event;
        static const char *deny[] = {"never-present"};
        int i;
        base_cfg(&cfg);
        cfg.property_denylist = deny; /* force the unpack/scrub/repack path */
        cfg.property_denylist_count = 1;
        start(&cfg);
        ph_props_init(&super);
        for (i = 0; i < PH_MAX_PROPS; i++) {
            char key[16];
            snprintf(key, sizeof(key), "super_%d", i);
            ph_props_set_int(&super, key, i);
        }
        ph_register(&super);
        ph_props_init(&event);
        ph_props_set_str(&event, "explicit_a", "kept-a");
        ph_props_set_str(&event, "explicit_b", "kept-b");
        ph_capture("explicit_priority", &event);
        ph_flush(2000);
        CHECK_CONTAINS(mock_batch(0), "\"explicit_a\":\"kept-a\"");
        CHECK_CONTAINS(mock_batch(0), "\"explicit_b\":\"kept-b\"");
        CHECK_NOT_CONTAINS(mock_batch(0), "\"super_23\":23");
        ph_shutdown();
    }

    /* --- rate limiter caps a burst of captures --- */
    {
        ph_config cfg;
        int i, passed;
        uint64_t dropped;
        base_cfg(&cfg);
        cfg.rate_limit_per_sec = 5; /* burst == 5 */
        start(&cfg);
        for (i = 0; i < 50; i++) ph_capture("spam", NULL);
        ph_flush(2000);
        dropped = ph_dropped_events();
        CHECK_MSG(dropped >= 40, "expected >=40 dropped, got %llu", (unsigned long long)dropped);
        CHECK_MSG(dropped <= 49, "expected <=49 dropped, got %llu", (unsigned long long)dropped);
        passed = 50 - (int)dropped;
        CHECK(passed >= 1 && passed <= 10);
        CHECK(mock_batch_count() == 1);
        CHECK(count_occurrences(mock_batch(0), "\"event\":\"spam\"") == passed);
        ph_shutdown();
    }
}
