/*
 * ph_wasm.c - the WebAssembly backend: a thin shim over host-initialized
 * posthog-js. Selected at compile time by __EMSCRIPTEN__.
 *
 * There is no network stack here: posthog-js owns batching, retry, offline, and
 * the reverse-proxy host. The shim's jobs are (a) validate the versioned host
 * descriptor installed by wasm/posthog-c-host.mjs (client, identity, privacy,
 * profile, and flag policy) before committing any C state; and (b)
 * serialize ph_props with the *same* encoder the native path uses, so event
 * scalar property shapes stay comparable across backends. posthog-js remains
 * the owner of timestamps, UUIDs, automatic properties, profiles, flags,
 * batching, retry, and persistence; native hot-path/delivery guarantees do not
 * apply to this synchronous bridge.
 *
 * The public API is implemented entirely here; the shared property/JSON code
 * (ph_props.c, ph_json.c, ph_str.c, ph_serialize.c) compiles into the wasm
 * module unchanged. ph_flush is a no-op; ph_shutdown releases only shim-owned
 * C state because posthog-js manages its own lifecycle.
 */
#if defined(__EMSCRIPTEN__)

#include "posthog.h"
#include "ph_exception_shared.h"
#include "ph_str.h"
#include "ph_util.h"

#include <emscripten.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define PH_WASM_API_KEY_CAP 96
#define PH_WASM_HOST_CAP 256
#define PH_WASM_RELEASE_CAP 96

/* Defined in ph_serialize.c (shared). Forward-declared so this TU doesn't pull
 * in the native-only internal header. */
void ph_serialize_props_object(const ph_props *p, ph_strbuf *out);

/* Typed host bridge. EM_JS works under strict -std=c11, and every host-owned
 * name is quoted so Closure cannot rename the ABI. The bridge uses the client
 * reference in the supported descriptor, never an assumed window.posthog. */
EM_JS(int, ph__wasm_js_validate_host,
      (const char *api_key_ptr, const char *host_ptr,
       const char *distinct_id_ptr, int person_profiles, int preload_flags,
       int send_flag_events, const char *release_ptr, int disable_geoip), {
    try {
        var win = globalThis["window"];
        var d = globalThis["__posthog_c_v1"] ||
                (win && win["__posthog_c_v1"]);
        var profiles = ["identified_only", "always", "never"];
        if (!d || d["abi"] !== 1 ||
            !globalThis["Object"]["isFrozen"](d) ||
            typeof d["checked_client"] !== "function" ||
            typeof d["with_alias"] !== "function") return 0;
        var client = d["checked_client"]();
        if (!client || client !== d["client"] ||
            typeof client["capture"] !== "function" ||
            typeof client["identify"] !== "function" ||
            typeof client["alias"] !== "function" ||
            typeof client["get_distinct_id"] !== "function" ||
            typeof client["reset"] !== "function" ||
            typeof client["group"] !== "function" ||
            typeof client["getFeatureFlag"] !== "function" ||
            typeof client["getFeatureFlagPayload"] !== "function" ||
            typeof client["reloadFeatureFlags"] !== "function" ||
            typeof client["register"] !== "function" ||
            typeof client["setPersonPropertiesForFlags"] !== "function") return 0;
        var apiKey = UTF8ToString(api_key_ptr);
        var host = UTF8ToString(host_ptr);
        var distinctId = UTF8ToString(distinct_id_ptr);
        var release = UTF8ToString(release_ptr);
        if (d["api_key"] !== apiKey || d["api_host"] !== host ||
            d["distinct_id"] !== distinctId ||
            globalThis["String"](client["get_distinct_id"]()) !== distinctId ||
            d["person_profiles"] !== profiles[person_profiles] ||
            !!d["preload_flags"] !== !!preload_flags ||
            !!d["send_feature_flag_events"] !== !!send_flag_events ||
            d["rate_policy"] !== "posthog-js" || d["release"] !== release ||
            d["finalizer_version"] !== 1 ||
            d["final_scrubber"] !== "posthog-js-before-send-v1") return 0;
        if (disable_geoip) {
            if (d["geoip_events"] !== "force-disable-v1" ||
                d["geoip_flags"] !== "proxy-inject-v1") return 0;
        } else if (d["geoip_events"] !== "default" ||
                   d["geoip_flags"] !== "default") return 0;
        if (Module["__posthog_c_bound_v1"] !== undefined) return 0;
        globalThis["Object"]["defineProperty"](
            Module, "__posthog_c_bound_v1",
            {"value": d, "writable": false, "configurable": true});
        return 1;
    } catch (_) { return 0; }
});

EM_JS(int, ph__wasm_js_capture,
      (const char *event_ptr, const char *props_ptr), {
    try {
        var d = Module["__posthog_c_bound_v1"];
        var client = d["checked_client"]();
        client["capture"](UTF8ToString(event_ptr),
            globalThis["JSON"]["parse"](UTF8ToString(props_ptr)));
        return 1;
    } catch (_) { return 0; }
});

EM_JS(int, ph__wasm_js_identify,
      (const char *id_ptr, const char *props_ptr, int never_profile), {
    try {
        var d = Module["__posthog_c_bound_v1"];
        var client = d["checked_client"]();
        var id = UTF8ToString(id_ptr);
        var props = globalThis["JSON"]["parse"](UTF8ToString(props_ptr));
        if (!never_profile) {
            client["identify"](id, props);
            return 1;
        }
        var previous = globalThis["String"](client["get_distinct_id"]());
        client["register"]({"$user_id": id, "distinct_id": id});
        client["setPersonPropertiesForFlags"](props, false);
        client["capture"]("$identify", {
            "distinct_id": id,
            "$anon_distinct_id": previous,
            "$set": props,
            "$process_person_profile": false
        });
        client["reloadFeatureFlags"]();
        return 1;
    } catch (_) { return 0; }
});

EM_JS(int, ph__wasm_js_alias,
      (const char *new_id_ptr, const char *old_id_ptr, int never_profile), {
    try {
        var d = Module["__posthog_c_bound_v1"];
        var client = d["checked_client"]();
        var alias = UTF8ToString(new_id_ptr);
        var original = UTF8ToString(old_id_ptr);
        d["with_alias"](alias, original, function() {
            if (never_profile) {
                client["capture"]("$create_alias", {
                    "alias": alias,
                    "distinct_id": original,
                    "$process_person_profile": false
                });
            } else {
                client["alias"](alias, original);
            }
        });
        return 1;
    } catch (_) { return 0; }
});

EM_JS(int, ph__wasm_js_get_distinct_id, (char *out, int cap), {
    try {
        var d = Module["__posthog_c_bound_v1"];
        var client = d["checked_client"]();
        var id = globalThis["String"](client["get_distinct_id"]());
        var Encoder = globalThis["TextEncoder"];
        var len = new Encoder()["encode"](id)["length"];
        stringToUTF8(id, out, cap);
        return len;
    } catch (_) { return -1; }
});

EM_JS(int, ph__wasm_js_reset, (void), {
    try {
        var d = Module["__posthog_c_bound_v1"];
        var client = d["checked_client"]();
        client["reset"]();
        return 1;
    } catch (_) { return 0; }
});

EM_JS(int, ph__wasm_js_group,
      (const char *type_ptr, const char *key_ptr, const char *props_ptr), {
    try {
        var d = Module["__posthog_c_bound_v1"];
        var client = d["checked_client"]();
        client["group"](UTF8ToString(type_ptr), UTF8ToString(key_ptr),
            globalThis["JSON"]["parse"](UTF8ToString(props_ptr)));
        return 1;
    } catch (_) { return 0; }
});

EM_JS(int, ph__wasm_js_capture_exception,
      (const char *props_ptr, const char *list_ptr), {
    try {
        var d = Module["__posthog_c_bound_v1"];
        var client = d["checked_client"]();
        var props = globalThis["JSON"]["parse"](UTF8ToString(props_ptr));
        props["$exception_list"] =
            globalThis["JSON"]["parse"](UTF8ToString(list_ptr));
        client["capture"]("$exception", props);
        return 1;
    } catch (_) { return 0; }
});

EM_JS(int, ph__wasm_js_is_feature_enabled,
      (const char *key_ptr, int fallback, int send_event), {
    try {
        var d = Module["__posthog_c_bound_v1"];
        var client = d["checked_client"]();
        var value = client["getFeatureFlag"](UTF8ToString(key_ptr),
                                              {"send_event": !!send_event});
        if (value === undefined || value === null) return fallback;
        return value === false ? 0 : (value ? 1 : 0);
    } catch (_) { return -1; }
});

EM_JS(int, ph__wasm_js_get_feature_flag,
      (const char *key_ptr, char *out, int cap, int send_event), {
    try {
        var d = Module["__posthog_c_bound_v1"];
        var client = d["checked_client"]();
        var value = client["getFeatureFlag"](UTF8ToString(key_ptr),
                                              {"send_event": !!send_event});
        if (value === undefined || value === null) return 0;
        var result = value === true ? "true"
            : (value === false ? "false" : globalThis["String"](value));
        if (out && cap > 0) stringToUTF8(result, out, cap);
        return 1;
    } catch (_) { return -1; }
});

EM_JS(int, ph__wasm_js_get_feature_flag_payload,
      (const char *key_ptr, char *out, int cap), {
    try {
        var d = Module["__posthog_c_bound_v1"];
        var client = d["checked_client"]();
        var value = client["getFeatureFlagPayload"](UTF8ToString(key_ptr));
        if (value === undefined || value === null) return 0;
        var result = globalThis["JSON"]["stringify"](value);
        if (out && cap > 0) stringToUTF8(result, out, cap);
        return 1;
    } catch (_) { return -1; }
});

EM_JS(int, ph__wasm_js_reload_feature_flags, (void), {
    try {
        var d = Module["__posthog_c_bound_v1"];
        var client = d["checked_client"]();
        client["reloadFeatureFlags"]();
        return 1;
    } catch (_) { return 0; }
});

EM_JS(void, ph__wasm_js_unbind_host, (void), {
    try { delete Module["__posthog_c_bound_v1"]; } catch (_) {}
});

static int g_initialized = 0;
static int g_enabled = 0;
static int g_identity_ok = 0;
static int g_person_profiles = PH_IDENTIFIED_ONLY;
static int g_send_feature_flag_events = 1;
static ph_before_send_fn g_before_send = NULL;
static ph_log_fn g_on_log = NULL;
static void *g_user_data = NULL;
static char g_denylist[PH_MAX_DENYLIST][PH_KEY_CAP];
static int g_denylist_count = 0;
static ph_props g_super;
static _Thread_local int g_in_callback;

/* Deterministic regression seam: the WASM harness links the production source
 * set directly and asks the next property serialization to take its OOM path.
 * Unreferenced production builds dead-strip this internal ph__ symbol. */
static int g_fail_next_props_serialize = 0;
void ph__wasm_test_fail_next_props_serialize(void) {
    g_fail_next_props_serialize = 1;
}

const char *ph_version(void) { return PH_VERSION_STRING; }

static void wasm_log(ph_log_level level, const char *message) {
    if (!g_on_log) return;
    g_in_callback++;
    g_on_log(level, message, g_user_data);
    g_in_callback--;
}

/* copy-capped / typed-value copy / remove-key / find-last-string live in
 * ph_util.c, shared with the native backend. The two denylist helpers bind this
 * backend's global denylist to that shared implementation. */
static void apply_denylist(ph_props *p) {
    ph_apply_denylist(p, g_denylist, g_denylist_count);
}

static int denylist_has(const char *key) {
    return ph_denylist_has(g_denylist, g_denylist_count, key);
}

static int scrub_props(const char *event, const ph_props *in, int include_super,
                       ph_props *out) {
    ph_props_merge(out, in, include_super ? &g_super : NULL);
    apply_denylist(out);
    if (g_before_send) {
        int keep;
        g_in_callback++;
        keep = g_before_send(event, out, g_user_data);
        g_in_callback--;
        if (!keep) return 0;
    }
    return 1;
}

void ph_config_defaults(ph_config *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->api_host = "https://us.i.posthog.com";
    cfg->flush_at = 20;
    cfg->flush_interval_ms = 30000;
    cfg->max_batch = 50;
    cfg->max_queue = 1000;
    cfg->request_timeout_ms = 10000;
    cfg->max_retries = 3;
    cfg->gzip = 1; /* honored on native; the wasm backend leaves delivery to posthog-js */
    cfg->enabled = 1;
    cfg->person_profiles = PH_IDENTIFIED_ONLY;
    cfg->send_feature_flag_events = 1;
    cfg->preload_flags = 1;
    cfg->disable_geoip = 0;
    cfg->max_batch_bytes = 1024 * 1024; /* native-only delivery cap */
}

/* Serialize props to a malloc'd JSON object string (caller frees). NULL means
 * serialization failed; callers must suppress the host operation rather than
 * silently substitute a schema-less object or hand partial JSON to JS. */
static char *props_to_json(const ph_props *p) {
    ph_strbuf b;
    ph_strbuf_init(&b);
    if (p)
        ph_serialize_props_object(p, &b);
    else
        ph_strbuf_append_cstr(&b, "{}");
    if (g_fail_next_props_serialize) {
        g_fail_next_props_serialize = 0;
        b.oom = 1;
    }
    if (b.oom || !b.data) {
        ph_strbuf_free(&b);
        wasm_log(PH_LOG_ERROR, "wasm: property serialization failed");
        return NULL;
    }
    return b.data;
}

static int string_over_cap(const char *s, size_t cap) {
    return s && strlen(s) >= cap;
}

static int normalize_api_host(const char *value, char *out, size_t cap) {
    const char *host = (value && value[0]) ? value
                                           : "https://us.i.posthog.com";
    size_t n = strlen(host);
    if (n == 0 || n >= cap || isspace((unsigned char)host[0]) ||
        isspace((unsigned char)host[n - 1])) return 0;
    if (strncmp(host, "http://", 7) != 0 &&
        strncmp(host, "https://", 8) != 0) return 0;
    while (n > 0 && host[n - 1] == '/') n--;
    if (n <= (strncmp(host, "https://", 8) == 0 ? 8u : 7u)) return 0;
    memcpy(out, host, n);
    out[n] = '\0';
    return 1;
}

static ph_result validate_config(const ph_config *cfg) {
    int i;
    char normalized_host[PH_WASM_HOST_CAP];
    if (!cfg) return PH_ERR_BADARG;
    if (!cfg->enabled) return PH_OK;
    if (!cfg->api_key || !cfg->api_key[0]) return PH_ERR_BADARG;
    if (!cfg->distinct_id || cfg->distinct_id[0] == '\0') return PH_ERR_BADARG;
    if (string_over_cap(cfg->api_key, PH_WASM_API_KEY_CAP) ||
        string_over_cap(cfg->distinct_id, PH_DISTINCT_ID_CAP) ||
        string_over_cap(cfg->release, PH_WASM_RELEASE_CAP) ||
        !normalize_api_host(cfg->api_host, normalized_host,
                            sizeof(normalized_host))) return PH_ERR_BADARG;
    if (cfg->person_profiles < PH_IDENTIFIED_ONLY ||
        cfg->person_profiles > PH_NEVER) return PH_ERR_BADARG;
    if (cfg->property_denylist_count < 0 ||
        cfg->property_denylist_count > PH_MAX_DENYLIST)
        return PH_ERR_BADARG;
    if (cfg->property_denylist_count > 0 && !cfg->property_denylist)
        return PH_ERR_BADARG;
    for (i = 0; cfg->property_denylist &&
                i < cfg->property_denylist_count; i++) {
        if (cfg->property_denylist[i] &&
            strlen(cfg->property_denylist[i]) >= PH_KEY_CAP)
            return PH_ERR_BADARG;
    }
    return PH_OK;
}

ph_result ph_init(const ph_config *cfg) {
    ph_result valid;
    int identity_ok = 0;
    int i;
    char normalized_host[PH_WASM_HOST_CAP];
    const char *release;

    if (g_initialized) return PH_ERR;
    valid = validate_config(cfg);
    if (valid != PH_OK) return valid;

    if (cfg->enabled) {
        /* Verify the supported host ABI before committing any C-side state. A
         * mismatch is retryable and cannot install callbacks or touch the client. */
        if (!normalize_api_host(cfg->api_host, normalized_host,
                                sizeof(normalized_host))) return PH_ERR_BADARG;
        release = cfg->release ? cfg->release : "";
        identity_ok = ph__wasm_js_validate_host(
            cfg->api_key, normalized_host, cfg->distinct_id,
            cfg->person_profiles, cfg->preload_flags,
            cfg->send_feature_flag_events, release, cfg->disable_geoip);
        if (!identity_ok) return PH_ERR;
    }

    /* Commit only after every fallible check has passed. */
    g_enabled = cfg->enabled ? 1 : 0;
    g_identity_ok = identity_ok;
    g_person_profiles = cfg->person_profiles;
    g_send_feature_flag_events = cfg->send_feature_flag_events ? 1 : 0;
    g_before_send = cfg->before_send;
    g_on_log = cfg->on_log;
    g_user_data = cfg->user_data;
    g_denylist_count = 0;
    ph_props_init(&g_super);
    for (i = 0; g_enabled && cfg->property_denylist &&
                i < cfg->property_denylist_count; i++) {
        if (cfg->property_denylist[i] && cfg->property_denylist[i][0])
            ph_copy_capped(g_denylist[g_denylist_count++], PH_KEY_CAP,
                           cfg->property_denylist[i]);
    }
    g_initialized = 1;

    return PH_OK;
}

ph_result ph_capture(const char *event, const ph_props *props) {
    char *json;
    char event_capped[PH_EVENT_NAME_CAP];
    ph_props clean;
    int truncated;
    if (!g_enabled || !g_identity_ok) return PH_ERR_DISABLED;
    if (!event || !event[0]) return PH_ERR_BADARG;
    truncated = strlen(event) >= sizeof(event_capped);
    ph_copy_capped(event_capped, sizeof(event_capped), event);
    if (!scrub_props(event_capped, props, 1, &clean)) return PH_OK; /* before_send dropped it */
    json = props_to_json(&clean);
    if (!json) return PH_ERR;
    if (!ph__wasm_js_capture(event_capped, json)) {
        free(json);
        wasm_log(PH_LOG_ERROR, "wasm: host capture failed");
        return PH_ERR;
    }
    free(json);
    return truncated ? PH_ERR_TRUNCATED : PH_OK;
}

void ph_identify(const char *distinct_id, const ph_props *set_props) {
    char *json;
    char id_capped[PH_DISTINCT_ID_CAP];
    ph_props clean;
    if (!g_enabled || !g_identity_ok || !distinct_id || !distinct_id[0]) return;
    ph_copy_capped(id_capped, sizeof(id_capped), distinct_id);
    if (!scrub_props("$identify", set_props, 0, &clean)) return;
    json = props_to_json(&clean);
    if (!json) return;
    if (!ph__wasm_js_identify(id_capped, json,
                              g_person_profiles == PH_NEVER))
        wasm_log(PH_LOG_ERROR, "wasm: host identify failed");
    free(json);
}

void ph_alias(const char *new_id, const char *old_id) {
    char new_capped[PH_DISTINCT_ID_CAP];
    char old_capped[PH_DISTINCT_ID_CAP];
    ph_props alias_props, clean;
    const char *clean_alias;
    if (!g_enabled || !g_identity_ok || !new_id || !new_id[0] ||
        !old_id || !old_id[0]) return;
    ph_copy_capped(new_capped, sizeof(new_capped), new_id);
    ph_copy_capped(old_capped, sizeof(old_capped), old_id);
    ph_props_init(&alias_props);
    ph_props_set_str(&alias_props, "alias", new_capped);
    if (!scrub_props("$create_alias", &alias_props, 0, &clean)) return;
    clean_alias = ph_props_find_last_str(&clean, "alias");
    if (!clean_alias || !clean_alias[0]) return;
    ph_copy_capped(new_capped, sizeof(new_capped), clean_alias);
    if (!ph__wasm_js_alias(new_capped, old_capped,
                           g_person_profiles == PH_NEVER))
        wasm_log(PH_LOG_ERROR, "wasm: host alias failed");
}

ph_result ph_get_distinct_id(char *out, int cap) {
    int len;
    if (!out || cap <= 0) return PH_ERR_BADARG;
    out[0] = '\0';
    if (!g_enabled || !g_identity_ok) return PH_ERR_DISABLED;
    len = ph__wasm_js_get_distinct_id(out, cap);
    if (len < 0) {
        wasm_log(PH_LOG_ERROR, "wasm: host distinct id read failed");
        return PH_ERR;
    }
    return len >= cap ? PH_ERR_TRUNCATED : PH_OK;
}

void ph_reset(void) {
    if (!g_enabled) return;
    ph_props_init(&g_super);
    if (!ph__wasm_js_reset()) wasm_log(PH_LOG_ERROR, "wasm: host reset failed");
}

void ph_group(const char *type, const char *key, const ph_props *set_props) {
    char *json;
    char type_capped[PH_KEY_CAP];
    char key_capped[PH_KEY_CAP];
    ph_props clean;
    if (!g_enabled || !g_identity_ok || !type || !type[0] ||
        !key || !key[0]) return;
    ph_copy_capped(type_capped, sizeof(type_capped), type);
    ph_copy_capped(key_capped, sizeof(key_capped), key);
    if (!scrub_props("$groupidentify", set_props, 0, &clean)) return;
    json = props_to_json(&clean);
    if (!json) return;
    if (!ph__wasm_js_group(type_capped, key_capped, json))
        wasm_log(PH_LOG_ERROR, "wasm: host group failed");
    free(json);
}

void ph_register(const ph_props *super_props) {
    int i, j;
    if (!g_enabled || !g_identity_ok || !super_props) return;
    for (i = 0; i < super_props->count; i++) {
        const ph_prop *src = &super_props->items[i];
        int found = 0;
        for (j = 0; j < g_super.count; j++) {
            if (strcmp(g_super.items[j].key, src->key) == 0) {
                g_super.items[j] = *src;
                found = 1;
                break;
            }
        }
        if (!found && g_super.count < PH_MAX_PROPS)
            g_super.items[g_super.count++] = *src;
    }
}

void ph_unregister(const char *key) {
    if (!g_enabled || !g_identity_ok || !key) return;
    ph_props_remove_key(&g_super, key);
}

static int prepare_exception_props(const ph_exception *ex, ph_props *clean,
                                   char *type, size_t type_cap,
                                   char *message, size_t message_cap,
                                   int *omit_function, int *omit_filename,
                                   int *omit_module, int *omit_frames) {
    ph_props base;
    ph__exception_prepare_base(ex, &base);
    ph_props_merge(clean, &base, &g_super);
    apply_denylist(clean);
    if (denylist_has("type")) ph_props_remove_key(clean, "$exception_type");
    if (denylist_has("message")) ph_props_remove_key(clean, "$exception_message");
    if (g_before_send) {
        int keep;
        g_in_callback++;
        keep = g_before_send("$exception", clean, g_user_data);
        g_in_callback--;
        if (!keep) return 0;
    }
    ph__exception_take_type_message(clean, type, type_cap, message, message_cap);

    *omit_function = denylist_has("function") ||
                     denylist_has("$exception_frame_function");
    *omit_filename = denylist_has("filename") ||
                     denylist_has("$exception_frame_filename");
    *omit_module = denylist_has("module") ||
                   denylist_has("$exception_frame_module");
    *omit_frames = denylist_has("frames") || denylist_has("stacktrace") ||
                   denylist_has("$exception_frames");
    return 1;
}

void ph_capture_exception(const ph_exception *ex) {
    char *json;
    ph_props clean;
    ph_strbuf list;
    char type[PH_EXCEPTION_FIELD_CAP];
    char message[PH_EXCEPTION_FIELD_CAP];
    int omit_function, omit_filename, omit_module, omit_frames;

    if (!g_enabled || !g_identity_ok || !ex) return;
    if (!prepare_exception_props(ex, &clean, type, sizeof(type), message,
                                 sizeof(message), &omit_function,
                                 &omit_filename, &omit_module, &omit_frames))
        return;

    json = props_to_json(&clean);
    if (!json) return;

    ph_strbuf_init(&list);
    ph__exception_build_list(&list, ex, type, message, omit_function,
                             omit_filename, omit_module, omit_frames, 0);
    if (list.oom || !list.data) {
        free(json);
        ph_strbuf_free(&list);
        wasm_log(PH_LOG_ERROR, "wasm: exception serialization failed");
        return;
    }

    if (!ph__wasm_js_capture_exception(json, list.data))
        wasm_log(PH_LOG_ERROR, "wasm: host exception capture failed");
    ph_strbuf_free(&list);
    free(json);
}

int ph_is_feature_enabled(const char *key, int fallback) {
    int value;
    if (!g_enabled || !g_identity_ok || !key) return fallback;
    value = ph__wasm_js_is_feature_enabled(key, fallback,
                                            g_send_feature_flag_events);
    if (value < 0) {
        wasm_log(PH_LOG_ERROR, "wasm: host feature flag read failed");
        return fallback;
    }
    return value;
}

ph_result ph_get_feature_flag(const char *key, char *out, int cap) {
    int found;
    if (out && cap > 0) out[0] = '\0';
    if (!g_enabled || !g_identity_ok || !key) return PH_ERR;
    found = ph__wasm_js_get_feature_flag(key, out, cap,
                                         g_send_feature_flag_events);
    if (found < 0) {
        wasm_log(PH_LOG_ERROR, "wasm: host feature flag read failed");
        return PH_ERR;
    }
    return found ? PH_OK : PH_ERR;
}

ph_result ph_get_feature_flag_payload(const char *key, char *out, int cap) {
    int found;
    if (out && cap > 0) out[0] = '\0';
    if (!g_enabled || !g_identity_ok || !key) return PH_ERR;
    found = ph__wasm_js_get_feature_flag_payload(key, out, cap);
    if (found < 0) {
        wasm_log(PH_LOG_ERROR, "wasm: host feature flag payload read failed");
        return PH_ERR;
    }
    return found ? PH_OK : PH_ERR;
}

void ph_reload_feature_flags(void) {
    if (!g_enabled || !g_identity_ok) return;
    if (!ph__wasm_js_reload_feature_flags())
        wasm_log(PH_LOG_ERROR, "wasm: host feature flag reload failed");
}

ph_result ph_reload_feature_flags_async(uint64_t *request_id) {
    if (request_id) *request_id = 0;
    if (!request_id) return PH_ERR_BADARG;
    if (!g_enabled || !g_identity_ok) return PH_ERR_DISABLED;
    return PH_ERR; /* posthog-js exposes no compatible per-request completion */
}

ph_feature_flag_reload_status ph_get_feature_flag_reload_status(
    uint64_t request_id) {
    (void)request_id;
    return PH_FEATURE_FLAG_RELOAD_UNKNOWN;
}

/* posthog-js owns delivery and its own drop accounting; nothing to report. */
uint64_t ph_dropped_events(void) { return 0; }

/* No SDK-owned queue or thread to drain - posthog-js manages its lifecycle. */
void ph_flush(int timeout_ms) { (void)timeout_ms; }
void ph_shutdown(void) {
    if (g_in_callback) return;
    ph__wasm_js_unbind_host();
    g_initialized = 0;
    g_enabled = 0;
    g_identity_ok = 0;
    g_person_profiles = PH_IDENTIFIED_ONLY;
    g_send_feature_flag_events = 1;
    g_before_send = NULL;
    g_on_log = NULL;
    g_user_data = NULL;
    g_denylist_count = 0;
    ph_props_init(&g_super);
    g_fail_next_props_serialize = 0;
}

#endif /* __EMSCRIPTEN__ */
