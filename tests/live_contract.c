/*
 * Opt-in real-service contract. Ordinary CI uses the deterministic mock; this
 * executable reads credentials only from its environment and is run manually
 * against a disposable PostHog project before release.
 */
#include "posthog.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_int g_transport_error;

static void contract_log(ph_log_level level, const char *msg, void *user) {
    (void)user;
    fprintf(stderr, "[posthog:%d] %s\n", (int)level, msg);
    if (level == PH_LOG_ERROR || strstr(msg, "failed") || strstr(msg, "dropped"))
        atomic_store(&g_transport_error, 1);
}

int main(void) {
    const char *key = getenv("POSTHOG_API_KEY");
    const char *host = getenv("POSTHOG_HOST");
    const char *did = getenv("POSTHOG_DISTINCT_ID");
    const char *flag_key = getenv("POSTHOG_FLAG_KEY");
    const char *expect_flag = getenv("POSTHOG_EXPECT_FLAG");
    const char *expect_payload = getenv("POSTHOG_EXPECT_PAYLOAD");
    ph_config cfg;
    ph_props props;
    char value[PH_VAL_CAP];
    char payload[4096];

    if (!key || !key[0]) {
        fprintf(stderr, "POSTHOG_API_KEY is required (use a disposable project)\n");
        return 2;
    }
    if (!host || !host[0]) host = "https://us.i.posthog.com";
    if (!did || !did[0]) did = "posthog-c-live-contract-install";
    atomic_init(&g_transport_error, 0);

    ph_config_defaults(&cfg);
    cfg.api_key = key;
    cfg.api_host = host;
    cfg.distinct_id = did;
    cfg.preload_flags = 0;
    cfg.flush_at = 1;
    cfg.max_retries = 1;
    cfg.on_log = contract_log;
    if (ph_init(&cfg) != PH_OK) {
        fprintf(stderr, "ph_init failed\n");
        return 3;
    }

    ph_props_init(&props);
    ph_props_set_str(&props, "contract", "batch");
    ph_props_set_str(&props, "sdk_version", ph_version());
    if (ph_capture("posthog_c_live_contract", &props) < PH_OK) {
        ph_shutdown();
        return 4;
    }

    if (flag_key && flag_key[0]) {
        ph_reload_feature_flags();
        if (ph_get_feature_flag(flag_key, value, sizeof value) != PH_OK) {
            fprintf(stderr, "expected flag '%s' was absent\n", flag_key);
            ph_shutdown();
            return 5;
        }
        if (expect_flag && strcmp(value, expect_flag) != 0) {
            fprintf(stderr, "flag mismatch: got '%s', expected '%s'\n",
                    value, expect_flag);
            ph_shutdown();
            return 6;
        }
        /* Reading above also queues the deduplicated exposure event. */
        if (expect_payload) {
            if (ph_get_feature_flag_payload(flag_key, payload, sizeof payload) != PH_OK ||
                strcmp(payload, expect_payload) != 0) {
                fprintf(stderr, "flag payload mismatch\n");
                ph_shutdown();
                return 7;
            }
        }
    }

    ph_flush(15000);
    if (ph_dropped_events() != 0 || atomic_load(&g_transport_error)) {
        fprintf(stderr, "live delivery reported terminal loss\n");
        ph_shutdown();
        return 8;
    }
    ph_shutdown();
    return 0;
}
