/*
 * Feature flags: a mocked /flags/ response feeds the cache, the accessors read
 * it (bool / variant / payload / fallback), and reading a flag emits exactly
 * one deduped $feature_flag_called (suppressible via send_feature_flag_events).
 */
#include "posthog.h"
#include "mock_transport.h"
#include "test_util.h"

#include <string.h>

static const char *FLAGS_JSON =
    "{\"flags\":{"
    "\"my-flag\":{\"key\":\"my-flag\",\"enabled\":true,"
    "\"metadata\":{\"payload\":\"{\\\"color\\\":\\\"red\\\"}\"}},"
    "\"off-flag\":{\"key\":\"off-flag\",\"enabled\":false},"
    "\"mv\":{\"key\":\"mv\",\"enabled\":true,\"variant\":\"test-a\"}"
    "},\"errorsWhileComputingFlags\":false}";

static const char *PARTIAL_FLAGS_JSON =
    "{\"flags\":{"
    "\"my-flag\":{\"key\":\"my-flag\",\"enabled\":false}"
    "},\"errorsWhileComputingFlags\":true}";

static const char *QUOTA_FLAGS_JSON =
    "{\"flags\":{},\"errorsWhileComputingFlags\":false,"
    "\"quotaLimited\":[\"feature_flags\"]}";

static void init_sdk(ph_config *cfg) {
    ph_config_defaults(cfg);
    cfg->api_key = "phc_flags";
    cfg->api_host = "http://127.0.0.1:9/x";
    cfg->distinct_id = "anon-f";
    cfg->flush_at = 100000;
    cfg->flush_interval_ms = 60000;
    cfg->preload_flags = 0;
    cfg->enabled = 1;
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

void suite_flags(void) {
    /* --- resolution: enabled / disabled / variant / payload / fallback --- */
    {
        ph_config cfg;
        char out[256];
        init_sdk(&cfg);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        mock_set_flags_response(FLAGS_JSON);
        ph_reload_feature_flags();
        CHECK_CONTAINS(mock_last_fetch_url(), "/flags?v=2");

        CHECK(ph_is_feature_enabled("my-flag", 0) == 1);
        CHECK(ph_is_feature_enabled("off-flag", 1) == 0);
        CHECK(ph_is_feature_enabled("unknown", 1) == 1); /* fallback */

        CHECK(ph_get_feature_flag("mv", out, sizeof(out)) == PH_OK);
        CHECK(strcmp(out, "test-a") == 0);
        CHECK(ph_get_feature_flag("my-flag", out, sizeof(out)) == PH_OK);
        CHECK(strcmp(out, "true") == 0); /* boolean flag -> "true" */
        CHECK(ph_get_feature_flag("unknown", out, sizeof(out)) == PH_ERR);

        CHECK(ph_get_feature_flag_payload("my-flag", out, sizeof(out)) == PH_OK);
        CHECK(strcmp(out, "{\"color\":\"red\"}") == 0);
        CHECK(ph_get_feature_flag_payload("off-flag", out, sizeof(out)) == PH_ERR);

        ph_shutdown();
    }

    /* --- $feature_flag_called is deduped across unchanged reloads --- */
    {
        ph_config cfg;
        int i, total = 0;
        init_sdk(&cfg);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        mock_set_flags_response(FLAGS_JSON);
        ph_reload_feature_flags();

        ph_is_feature_enabled("my-flag", 0);
        ph_is_feature_enabled("my-flag", 0);
        ph_get_feature_flag("my-flag", NULL, 0);
        ph_flush(2000);

        /* A normal refresh of the same value must preserve the exposure latch. */
        ph_reload_feature_flags();
        ph_is_feature_enabled("my-flag", 0);
        ph_flush(2000);

        for (i = 0; i < mock_batch_count(); i++)
            total += count_occurrences(mock_batch(i), "\"event\":\"$feature_flag_called\"");
        CHECK_MSG(total == 1, "expected 1 $feature_flag_called, got %d", total);
        CHECK_CONTAINS(mock_batch(0), "\"$feature_flag\":\"my-flag\"");
        CHECK_CONTAINS(mock_batch(0), "\"$feature_flag_response\":true");
        ph_shutdown();
    }

    /* --- partial errors merge; quota responses retain same-context values --- */
    {
        ph_config cfg;
        char out[64];
        init_sdk(&cfg);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        mock_set_flags_response(FLAGS_JSON);
        ph_reload_feature_flags();

        mock_set_flags_response(PARTIAL_FLAGS_JSON);
        ph_reload_feature_flags();
        CHECK(ph_is_feature_enabled("my-flag", 1) == 0); /* returned value updated */
        CHECK(ph_get_feature_flag("mv", out, sizeof(out)) == PH_OK); /* absent value retained */
        CHECK(strcmp(out, "test-a") == 0);

        mock_set_flags_response(QUOTA_FLAGS_JSON);
        ph_reload_feature_flags();
        CHECK(ph_is_feature_enabled("my-flag", 1) == 0);
        CHECK(ph_get_feature_flag("mv", out, sizeof(out)) == PH_OK);
        CHECK(strcmp(out, "test-a") == 0);
        ph_shutdown();
    }

    /* --- identity and group changes synchronously invalidate old values --- */
    {
        ph_config cfg;
        init_sdk(&cfg);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        mock_set_flags_response(FLAGS_JSON);
        ph_reload_feature_flags();
        CHECK(ph_is_feature_enabled("my-flag", 0) == 1);

        mock_set_status(500); /* async refetch cannot repopulate the cache */
        ph_reset();
        CHECK(ph_is_feature_enabled("my-flag", 0) == 0);

        mock_set_status(200);
        ph_reload_feature_flags();
        CHECK(ph_is_feature_enabled("my-flag", 0) == 1);
        mock_set_status(500);
        ph_group("company", "one", NULL);
        CHECK(ph_is_feature_enabled("my-flag", 0) == 0);

        mock_set_status(200);
        ph_reload_feature_flags();
        CHECK(ph_is_feature_enabled("my-flag", 0) == 1);
        mock_set_status(500);
        ph_identify("account-2", NULL);
        CHECK(ph_is_feature_enabled("my-flag", 0) == 0);

        mock_set_status(200);
        ph_shutdown();
    }

    /* --- identify/group properties override remote evaluation immediately --- */
    {
        ph_config cfg;
        ph_props person, group;
        init_sdk(&cfg);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        mock_set_flags_response(FLAGS_JSON);

        ph_props_init(&person);
        ph_props_set_str(&person, "plan", "pro");
        ph_identify("account-with-props", &person);
        ph_reload_feature_flags();
        CHECK_CONTAINS(mock_last_fetch_body(), "\"person_properties\":{\"plan\":\"pro\"}");

        ph_props_init(&group);
        ph_props_set_int(&group, "seats", 25);
        ph_group("company", "acme", &group);
        ph_reload_feature_flags();
        CHECK_CONTAINS(mock_last_fetch_body(), "\"groups\":{\"company\":\"acme\"}");
        CHECK_CONTAINS(mock_last_fetch_body(), "\"group_properties\":{\"company\":{\"seats\":25}}");
        ph_shutdown();
    }

    /* --- send_feature_flag_events = 0 suppresses the exposure event --- */
    {
        ph_config cfg;
        int i, total = 0;
        init_sdk(&cfg);
        cfg.send_feature_flag_events = 0;
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        mock_set_flags_response(FLAGS_JSON);
        ph_reload_feature_flags();

        ph_is_feature_enabled("my-flag", 0);
        ph_flush(2000);

        for (i = 0; i < mock_batch_count(); i++)
            total += count_occurrences(mock_batch(i), "$feature_flag_called");
        CHECK(total == 0);
        ph_shutdown();
    }
}
