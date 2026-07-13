/*
 * Feature flags: a mocked /flags/ response feeds the cache, the accessors read
 * it (bool / variant / payload / fallback), and reading a flag emits exactly
 * one deduped $feature_flag_called (suppressible via send_feature_flag_events).
 */
#include "posthog.h"
#include "ph_internal.h"
#include "mock_transport.h"
#include "test_util.h"

#include <stdatomic.h>
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

static ph_feature_flag_reload_status wait_reload_terminal(uint64_t request_id) {
    int i;
    ph_feature_flag_reload_status status = PH_FEATURE_FLAG_RELOAD_PENDING;
    for (i = 0; i < 2000; i++) {
        status = ph_get_feature_flag_reload_status(request_id);
        if (status != PH_FEATURE_FLAG_RELOAD_PENDING) return status;
        ph_sleep_ms(1);
    }
    return status;
}

static ph_feature_flag_reload_status wait_current_reload(void) {
    ph_feature_flag_reload_status status;
    uint64_t request_id;
    do {
        if (ph_reload_feature_flags_async(&request_id) != PH_OK)
            return PH_FEATURE_FLAG_RELOAD_UNKNOWN;
        status = wait_reload_terminal(request_id);
    } while (status == PH_FEATURE_FLAG_RELOAD_SUPERSEDED);
    return status;
}

static void reload_from_log(ph_log_level level, const char *msg, void *user) {
    atomic_int *calls = (atomic_int *)user;
    (void)level;
    (void)msg;
    atomic_fetch_add(calls, 1);
    ph_reload_feature_flags(); /* sender callback: enqueue/attach, never wait */
}

void suite_flags(void) {
    /* --- async argument/disabled behavior is side-effect free --- */
    {
        uint64_t request_id = 99;
        CHECK(ph_reload_feature_flags_async(NULL) == PH_ERR_BADARG);
        CHECK(ph_reload_feature_flags_async(&request_id) == PH_ERR_DISABLED);
        CHECK(request_id == 0);
        CHECK(ph_get_feature_flag_reload_status(99) ==
              PH_FEATURE_FLAG_RELOAD_UNKNOWN);
    }

    /* --- async requests are pending, same-context coalesced, then successful --- */
    {
        ph_config cfg;
        uint64_t first = 0, coalesced = 0;
        init_sdk(&cfg);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        mock_set_flags_response(FLAGS_JSON);
        mock_set_fetch_blocked(1);
        CHECK(ph_reload_feature_flags_async(&first) == PH_OK);
        CHECK(first != 0);
        CHECK(mock_wait_fetch_count(1, 2000));
        CHECK(ph_get_feature_flag_reload_status(first) ==
              PH_FEATURE_FLAG_RELOAD_PENDING);
        CHECK(ph_reload_feature_flags_async(&coalesced) == PH_OK);
        CHECK(coalesced == first);
        CHECK(mock_fetch_count() == 1);
        mock_set_fetch_blocked(0);
        CHECK(wait_reload_terminal(first) == PH_FEATURE_FLAG_RELOAD_SUCCESS);
        CHECK(mock_fetch_count() == 1);
        ph_shutdown();
    }

    /* --- transport, invalid JSON, partial evaluation, and callback safety --- */
    {
        ph_config cfg;
        uint64_t request_id = 0;
        atomic_int log_calls;
        atomic_init(&log_calls, 0);
        init_sdk(&cfg);
        cfg.on_log = reload_from_log;
        cfg.user_data = &log_calls;
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();

        mock_set_status(500);
        CHECK(ph_reload_feature_flags_async(&request_id) == PH_OK);
        CHECK(wait_reload_terminal(request_id) == PH_FEATURE_FLAG_RELOAD_FAILED);
        CHECK(atomic_load(&log_calls) > 0);

        mock_set_status(200);
        mock_set_flags_response("not-json");
        CHECK(ph_reload_feature_flags_async(&request_id) == PH_OK);
        CHECK(wait_reload_terminal(request_id) == PH_FEATURE_FLAG_RELOAD_FAILED);

        mock_set_flags_response(QUOTA_FLAGS_JSON);
        CHECK(ph_reload_feature_flags_async(&request_id) == PH_OK);
        CHECK(wait_reload_terminal(request_id) == PH_FEATURE_FLAG_RELOAD_FAILED);

        mock_set_flags_response(FLAGS_JSON);
        CHECK(ph_reload_feature_flags_async(&request_id) == PH_OK);
        CHECK(wait_reload_terminal(request_id) == PH_FEATURE_FLAG_RELOAD_SUCCESS);
        mock_set_flags_response(PARTIAL_FLAGS_JSON);
        CHECK(ph_reload_feature_flags_async(&request_id) == PH_OK);
        CHECK(wait_reload_terminal(request_id) == PH_FEATURE_FLAG_RELOAD_FAILED);
        CHECK(ph_is_feature_enabled("my-flag", 1) == 0); /* returned value merged */
        {
            char value[32];
            CHECK(ph_get_feature_flag("mv", value, sizeof(value)) == PH_OK);
            CHECK(strcmp(value, "test-a") == 0); /* absent value retained */
        }
        ph_shutdown();
    }

    /* --- an identity-context change supersedes an in-flight request --- */
    {
        ph_config cfg;
        uint64_t old_request = 0, new_request = 0;
        init_sdk(&cfg);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        mock_set_flags_response(FLAGS_JSON);
        mock_set_fetch_blocked(1);
        CHECK(ph_reload_feature_flags_async(&old_request) == PH_OK);
        CHECK(mock_wait_fetch_count(1, 2000));
        ph_reset(); /* changes identity without queueing a second control mutation */
        CHECK(ph_get_feature_flag_reload_status(old_request) ==
              PH_FEATURE_FLAG_RELOAD_SUPERSEDED);
        CHECK(ph_reload_feature_flags_async(&new_request) == PH_OK);
        CHECK(new_request != old_request);
        CHECK(ph_get_feature_flag_reload_status(new_request) ==
              PH_FEATURE_FLAG_RELOAD_PENDING);
        mock_set_fetch_blocked(0);
        CHECK(mock_wait_fetch_count(2, 2000));
        CHECK(wait_reload_terminal(new_request) == PH_FEATURE_FLAG_RELOAD_SUCCESS);
        CHECK(ph_get_feature_flag_reload_status(old_request) ==
              PH_FEATURE_FLAG_RELOAD_SUPERSEDED);
        CHECK(mock_fetch_count() == 2);
        ph_shutdown();
    }

    /* --- terminal history is bounded; lifecycle tokens never alias --- */
    {
        ph_config cfg;
        uint64_t ids[PH_FLAG_RELOAD_HISTORY_CAP + 1];
        uint64_t next_lifecycle = 0;
        int i;
        init_sdk(&cfg);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        mock_set_flags_response(FLAGS_JSON);
        for (i = 0; i < PH_FLAG_RELOAD_HISTORY_CAP + 1; i++) {
            CHECK(ph_reload_feature_flags_async(&ids[i]) == PH_OK);
            CHECK(wait_reload_terminal(ids[i]) == PH_FEATURE_FLAG_RELOAD_SUCCESS);
        }
        CHECK(ph_get_feature_flag_reload_status(ids[0]) ==
              PH_FEATURE_FLAG_RELOAD_UNKNOWN);
        CHECK(ph_get_feature_flag_reload_status(ids[PH_FLAG_RELOAD_HISTORY_CAP]) ==
              PH_FEATURE_FLAG_RELOAD_SUCCESS);
        ph_shutdown();

        init_sdk(&cfg);
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        mock_set_flags_response(FLAGS_JSON);
        CHECK(ph_get_feature_flag_reload_status(ids[PH_FLAG_RELOAD_HISTORY_CAP]) ==
              PH_FEATURE_FLAG_RELOAD_UNKNOWN);
        CHECK(ph_reload_feature_flags_async(&next_lifecycle) == PH_OK);
        CHECK(next_lifecycle != ids[PH_FLAG_RELOAD_HISTORY_CAP]);
        CHECK(wait_reload_terminal(next_lifecycle) ==
              PH_FEATURE_FLAG_RELOAD_SUCCESS);
        ph_shutdown();
    }

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
        CHECK_NOT_CONTAINS(mock_last_fetch_body(), "\"geoip_disable\"");

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

    /* --- GeoIP opt-out is carried on every native flag evaluation request --- */
    {
        ph_config cfg;
        init_sdk(&cfg);
        cfg.disable_geoip = 1;
        CHECK(ph_init(&cfg) == PH_OK);
        mock_reset();
        mock_install();
        mock_set_flags_response(FLAGS_JSON);
        ph_reload_feature_flags();
        CHECK_CONTAINS(mock_last_fetch_body(), "\"geoip_disable\":true");
        ph_identify("geo-account", NULL);
        ph_reload_feature_flags();
        CHECK_CONTAINS(mock_last_fetch_body(), "\"geoip_disable\":true");
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
        CHECK(wait_current_reload() == PH_FEATURE_FLAG_RELOAD_FAILED);

        mock_set_status(200);
        ph_reload_feature_flags();
        CHECK(ph_is_feature_enabled("my-flag", 0) == 1);
        mock_set_status(500);
        ph_group("company", "one", NULL);
        CHECK(ph_is_feature_enabled("my-flag", 0) == 0);
        CHECK(wait_current_reload() == PH_FEATURE_FLAG_RELOAD_FAILED);

        mock_set_status(200);
        ph_reload_feature_flags();
        CHECK(ph_is_feature_enabled("my-flag", 0) == 1);
        mock_set_status(500);
        ph_identify("account-2", NULL);
        CHECK(ph_is_feature_enabled("my-flag", 0) == 0);
        CHECK(wait_current_reload() == PH_FEATURE_FLAG_RELOAD_FAILED);

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
