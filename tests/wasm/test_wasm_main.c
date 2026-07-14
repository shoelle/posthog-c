/*
 * The C side of the WASM behavioral test. Exposed as wasm_run_test() (not main)
 * so the Node harness can install its versioned host descriptor, spin up the runtime,
 * then call in - deterministic ordering, no reliance on auto-run timing.
 */
#include "posthog.h"

#include <emscripten.h>
#include <string.h>

/* Internal deterministic seam provided by ph_wasm.c. It is deliberately not
 * part of the installed public header. */
void ph__wasm_test_fail_next_props_serialize(void);

EM_JS(void, wasm_install_throwing_descriptor, (void), {
    var win = globalThis["window"];
    Module["__posthog_c_saved_public_v1"] =
        globalThis["__posthog_c_v1"] || (win && win["__posthog_c_v1"]);
    var bad = {};
    globalThis["Object"]["defineProperty"](bad, "abi", {
        "get": function() { throw new Error("test getter"); }
    });
    globalThis["Object"]["freeze"](bad);
    globalThis["__posthog_c_v1"] = bad;
    if (win) win["__posthog_c_v1"] = bad;
});

EM_JS(void, wasm_restore_public_descriptor, (void), {
    var win = globalThis["window"];
    var saved = Module["__posthog_c_saved_public_v1"];
    globalThis["__posthog_c_v1"] = saved;
    if (win) win["__posthog_c_v1"] = saved;
    delete Module["__posthog_c_saved_public_v1"];
});

EM_JS(void, wasm_replace_public_descriptor, (void), {
    var win = globalThis["window"];
    var bad = globalThis["Object"]["freeze"]({
        "client": {"capture": function() { throw new Error("unvalidated client"); }}
    });
    globalThis["__posthog_c_v1"] = bad;
    if (win) win["__posthog_c_v1"] = bad;
});

EM_JS(void, wasm_mutate_bound_finalizer, (void), {
    var d = Module["__posthog_c_bound_v1"];
    Module["__posthog_c_saved_finalizer_v1"] = d["client"]["config"]["before_send"];
    d["client"]["config"]["before_send"] = [];
});

EM_JS(void, wasm_restore_bound_finalizer, (void), {
    var d = Module["__posthog_c_bound_v1"];
    d["client"]["config"]["before_send"] =
        Module["__posthog_c_saved_finalizer_v1"];
    delete Module["__posthog_c_saved_finalizer_v1"];
});

static int log_count;

static void wasm_log(ph_log_level level, const char *message, void *user) {
    (void)level;
    (void)message;
    (void)user;
    log_count++;
}

static void matching_host_config(ph_config *cfg) {
    ph_config_defaults(cfg);
    cfg->api_key = "phc_wasm";
    cfg->api_host = "https://us.i.posthog.com////"; /* normalization is shared */
    cfg->distinct_id = "install-abc";
    cfg->person_profiles = PH_NEVER;
    cfg->send_feature_flag_events = 0;
    cfg->release = "wasm-release@1";
    cfg->disable_geoip = 1;
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
    matching_host_config(&cfg);
    cfg.api_key = NULL;
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

    wasm_install_throwing_descriptor();
    if (ph_init(&cfg) != PH_ERR) failures++;
    wasm_restore_public_descriptor();
    if (ph_capture("failed_throwing_host_init", NULL) != PH_ERR_DISABLED)
        failures++;

    cfg.api_key = "phc_other";
    if (ph_init(&cfg) != PH_ERR) failures++;
    cfg.api_key = "phc_wasm";
    cfg.api_host = "https://eu.i.posthog.com";
    if (ph_init(&cfg) != PH_ERR) failures++;
    cfg.api_host = "https://us.i.posthog.com////";
    cfg.distinct_id = "wrong-install-id";
    if (ph_init(&cfg) != PH_ERR) failures++;
    if (ph_capture("failed_identity_init", NULL) != PH_ERR_DISABLED) failures++;
    cfg.distinct_id = "install-abc";
    cfg.person_profiles = PH_ALWAYS;
    if (ph_init(&cfg) != PH_ERR) failures++;
    cfg.person_profiles = PH_NEVER;
    cfg.preload_flags = 0;
    if (ph_init(&cfg) != PH_ERR) failures++;
    cfg.preload_flags = 1;
    cfg.send_feature_flag_events = 1;
    if (ph_init(&cfg) != PH_ERR) failures++;
    cfg.send_feature_flag_events = 0;
    cfg.release = "wrong-release";
    if (ph_init(&cfg) != PH_ERR) failures++;
    cfg.release = "wasm-release@1";
    cfg.disable_geoip = 0;
    if (ph_init(&cfg) != PH_ERR) failures++;
    cfg.disable_geoip = 1;
    if (ph_init(&cfg) != PH_OK) failures++;
    ph_shutdown();

    matching_host_config(&cfg);
    cfg.before_send = wasm_before_send;
    cfg.on_log = wasm_log;
    cfg.property_denylist = denylist;
    cfg.property_denylist_count = 2;
    if (ph_init(&cfg) != PH_OK) failures++;
    wasm_replace_public_descriptor(); /* bridges must stay on the pinned client */

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
    ph_capture("host_drop_envelope", NULL);
    ph_props_init(&p);
    ph_props_set_str(&p, "distinct_id", "other-id"); /* SDK-owned: stripped, not a spoof */
    ph_props_set_str(&p, "$lib", "shadow-lib");      /* SDK-owned: stripped */
    ph_capture("caller_redirect_identity", &p);
    ph_capture("host_redirect_identity", NULL);
    ph_props_init(&p);
    ph_props_set_str(&p, "release", "caller-release@9"); /* overrides config release */
    ph_capture("caller_release_wins", &p);

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
    ph_alias("alias-9", "acct-9");
    ph_alias("alias-legacy", "legacy-old");
    ph_alias("", "acct-9");
    ph_alias("alias-empty-old", "");

    ph_props_init(&p);
    ph_props_set_int(&p, "players", 4);
    ph_props_set_str(&p, "token", "group-token");
    ph_group("game", "asteroids", &p);
    ph_group("", "asteroids", &p);
    ph_group("game", "", &p);

    if (ph_is_feature_enabled("missing", 1)) ph_capture("missing_fallback_true", NULL);
    if (ph_get_feature_flag("off", flag, sizeof(flag)) == PH_OK &&
        strcmp(flag, "false") == 0)
        ph_capture("false_flag_ok", NULL);
    {
        uint64_t request_id = 99;
        if (ph_reload_feature_flags_async(&request_id) != PH_ERR ||
            request_id != 0 ||
            ph_get_feature_flag_reload_status(99) !=
                PH_FEATURE_FLAG_RELOAD_UNKNOWN)
            failures++;
    }

    {
        ph_stackframe frames[2];
        ph_props extra;
        ph_exception ex;

        memset(frames, 0, sizeof(frames));
        frames[0].function = "sim::step";
        frames[0].filename = "sim.cpp";
        frames[0].module = "engine-module"; /* denylisted by field name */
        frames[0].lineno = 412;
        frames[0].in_app = 1;
        frames[1].function = "main";
        frames[1].filename = "main.cpp";
        frames[1].module = "app-module";
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

    wasm_mutate_bound_finalizer();
    if (ph_capture("mutated_host_contract", NULL) != PH_ERR) failures++;
    wasm_restore_bound_finalizer();
    if (ph_capture("restored_host_contract", NULL) != PH_OK) failures++;

    if (log_count < 5) failures++; /* serialization + host integrity failures diagnosed */

    return failures;
}

int main(void) { return 0; }
