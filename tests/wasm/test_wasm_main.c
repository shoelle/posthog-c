/*
 * The C side of the WASM behavioral test. Exposed as wasm_run_test() (not main)
 * so the Node harness can install its window.posthog mock, spin up the runtime,
 * then call in - deterministic ordering, no reliance on auto-run timing.
 */
#include "posthog.h"

#include <emscripten.h>
#include <string.h>

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

static int wasm_before_send(const char *event, ph_props *props, void *user) {
    (void)user;
    if (strcmp(event, "drop_me") == 0) return 0;
    remove_key(props, "secret");
    remove_key(props, "$exception_message");
    ph_props_set_bool(props, "scrubbed", 1);
    if (strcmp(event, "$exception") == 0)
        ph_props_set_str(props, "$exception_message", "redacted");
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void wasm_run_test(void) {
    ph_config cfg;
    ph_props p;
    static const char *denylist[] = {"token"};
    char flag[16];
    char distinct_id[PH_DISTINCT_ID_CAP];

    ph_config_defaults(&cfg);
    cfg.api_key = "phc_wasm";
    cfg.distinct_id = "install-abc"; /* must match the harness bootstrap id */
    cfg.before_send = wasm_before_send;
    cfg.property_denylist = denylist;
    cfg.property_denylist_count = 1;
    ph_init(&cfg);
    if (ph_get_distinct_id(distinct_id, sizeof(distinct_id)) == PH_OK &&
        strcmp(distinct_id, "install-abc") == 0)
        ph_capture("distinct_id_getter_ok", NULL);

    ph_props_init(&p);
    ph_props_set_str(&p, "super_keep", "yes");
    ph_props_set_str(&p, "token", "super-token");
    ph_props_set_str(&p, "secret", "super-secret");
    ph_register(&p);

    ph_props_init(&p);
    ph_props_set_str(&p, "weapon", "sword");
    ph_props_set_int(&p, "level", 3);
    ph_props_set_double(&p, "score", 1.5);
    ph_props_set_bool(&p, "alive", 1);
    ph_props_set_str(&p, "token", "do-not-send");
    ph_props_set_str(&p, "secret", "also-hidden");
    ph_capture("level_started", &p);
    ph_capture("drop_me", NULL);

    ph_props_init(&p);
    ph_props_set_str(&p, "plan", "pro");
    ph_props_set_str(&p, "token", "identify-token");
    ph_identify("acct-9", &p);

    ph_props_init(&p);
    ph_props_set_int(&p, "players", 4);
    ph_props_set_str(&p, "token", "group-token");
    ph_group("game", "asteroids", &p);

    if (ph_is_feature_enabled("missing", 1)) ph_capture("missing_fallback_true", NULL);
    if (ph_get_feature_flag("off", flag, sizeof(flag)) == PH_OK &&
        strcmp(flag, "false") == 0)
        ph_capture("false_flag_ok", NULL);

    {
        ph_exception ex;
        memset(&ex, 0, sizeof(ex));
        ex.type = "NativeAssertion";
        ex.message = "contains pii";
        ex.handled = 1;
        ph_capture_exception(&ex);
    }
}

int main(void) { return 0; }
