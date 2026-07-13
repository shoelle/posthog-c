/*
 * The C side of the WASM behavioral test. Exposed as wasm_run_test() (not main)
 * so the Node harness can install its window.posthog mock, spin up the runtime,
 * then call in - deterministic ordering, no reliance on auto-run timing.
 */
#include "posthog.h"

#include <emscripten.h>
#include <string.h>

/* Internal deterministic seam provided by ph_wasm.c. It is deliberately not
 * part of the installed public header. */
void ph__wasm_test_fail_next_props_serialize(void);

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
int wasm_run_test(void) {
    ph_config cfg;
    ph_props p;
    ph_config second;
    static const char *denylist[] = {"token", "module"};
    const char *invalid_denylist[1];
    char overcap_denylist_key[PH_KEY_CAP + 1];
    char flag[16];
    char distinct_id[PH_DISTINCT_ID_CAP];
    int failures = 0;

    /* Disabled initialization still owns the singleton until shutdown. */
    ph_config_defaults(&cfg);
    cfg.enabled = 0;
    if (ph_init(&cfg) != PH_OK) failures++;
    if (ph_init(&cfg) != PH_ERR) failures++;
    if (ph_capture("disabled_init", NULL) != PH_ERR_DISABLED) failures++;
    ph_shutdown();

    /* Failed argument/host validation commits no state and remains retryable. */
    ph_config_defaults(&cfg);
    cfg.distinct_id = "install-abc";
    if (ph_init(&cfg) != PH_ERR_BADARG) failures++;
    if (ph_capture("failed_badarg_init", NULL) != PH_ERR_DISABLED) failures++;
    cfg.api_key = "phc_wasm";
    memset(overcap_denylist_key, 'x', sizeof(overcap_denylist_key) - 1);
    overcap_denylist_key[sizeof(overcap_denylist_key) - 1] = '\0';
    invalid_denylist[0] = overcap_denylist_key;
    cfg.property_denylist = invalid_denylist;
    cfg.property_denylist_count = 1;
    if (ph_init(&cfg) != PH_ERR_BADARG) failures++;
    if (ph_capture("failed_denylist_init", NULL) != PH_ERR_DISABLED) failures++;
    cfg.property_denylist = NULL;
    cfg.property_denylist_count = 0;
    cfg.distinct_id = "wrong-install-id";
    if (ph_init(&cfg) != PH_ERR) failures++;
    if (ph_capture("failed_identity_init", NULL) != PH_ERR_DISABLED) failures++;
    cfg.distinct_id = "install-abc";
    if (ph_init(&cfg) != PH_OK) failures++;
    ph_shutdown();

    ph_config_defaults(&cfg);
    cfg.api_key = "phc_wasm";
    cfg.distinct_id = "install-abc"; /* must match the harness bootstrap id */
    cfg.before_send = wasm_before_send;
    cfg.property_denylist = denylist;
    cfg.property_denylist_count = 2;
    if (ph_init(&cfg) != PH_OK) failures++;

    ph_props_init(&p);
    ph_props_set_str(&p, "super_keep", "yes");
    ph_props_set_str(&p, "token", "super-token");
    ph_props_set_str(&p, "secret", "super-secret");
    ph_register(&p);

    /* Reconfiguration is rejected before it can clear callbacks, denylist, or
     * already-registered super properties on the live singleton. */
    ph_config_defaults(&second);
    second.enabled = 0;
    if (ph_init(&second) != PH_ERR) failures++;

    if (ph_get_distinct_id(distinct_id, sizeof(distinct_id)) == PH_OK &&
        strcmp(distinct_id, "install-abc") == 0)
        ph_capture("distinct_id_getter_ok", NULL);
    else
        failures++;

    ph_props_init(&p);
    ph_props_set_str(&p, "weapon", "sword");
    ph_props_set_int(&p, "level", 3);
    ph_props_set_double(&p, "score", 1.5);
    ph_props_set_bool(&p, "alive", 1);
    ph_props_set_str(&p, "token", "do-not-send");
    ph_props_set_str(&p, "secret", "also-hidden");
    ph_capture("level_started", &p);
    ph_capture("drop_me", NULL);

    /* Every JSON-using facade call fails closed when serialization fails. */
    ph_props_init(&p);
    ph_props_set_str(&p, "required", "present");
    ph__wasm_test_fail_next_props_serialize();
    if (ph_capture("oom_capture", &p) != PH_ERR) failures++;
    ph__wasm_test_fail_next_props_serialize();
    ph_identify("oom-identify", &p);
    ph__wasm_test_fail_next_props_serialize();
    ph_group("game", "oom-group", &p);

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
        ph_stackframe frames[2];
        ph_props extra;
        ph_exception ex;

        memset(frames, 0, sizeof(frames));
        frames[0].function = "sim::step";
        frames[0].filename = "sim.cpp";
        frames[0].module = "private-engine"; /* denylisted by field name */
        frames[0].lineno = 412;
        frames[0].in_app = 1;
        frames[1].function = "main";
        frames[1].filename = "main.cpp";
        frames[1].module = "private-app";
        frames[1].lineno = 20;
        frames[1].in_app = 1;

        ph_props_init(&extra);
        ph_props_set_str(&extra, "exception_keep", "yes");
        ph_props_set_str(&extra, "token", "exception-token");
        ph_props_set_str(&extra, "secret", "exception-secret");

        memset(&ex, 0, sizeof(ex));
        ex.type = "NativeAssertion";
        ex.message = "contains pii";
        ex.handled = 1;
        ex.synthetic = 1;
        ex.frames = frames;
        ex.frame_count = 2;
        ex.extra = &extra;
        ph_capture_exception(&ex);

        ex.type = "OOMException";
        ph__wasm_test_fail_next_props_serialize();
        ph_capture_exception(&ex);
    }

    return failures;
}

int main(void) { return 0; }
