/*
 * ph_wasm.c - the WebAssembly backend: a thin shim over the browser's already
 * loaded posthog-js. Selected at compile time by __EMSCRIPTEN__.
 *
 * There is no network stack here: posthog-js owns batching, retry, offline, and
 * the reverse-proxy host. The shim's jobs are (a) verify at init that the host
 * bootstrapped posthog-js with the same anonymous install id we were handed
 * (via a host-owned window.__posthog_c_distinct_id), disabling capture on a
 * mismatch rather than splitting a user's web and native timelines; and (b)
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
#include <stdlib.h>
#include <string.h>

/* Defined in ph_serialize.c (shared). Forward-declared so this TU doesn't pull
 * in the native-only internal header. */
void ph_serialize_props_object(const ph_props *p, ph_strbuf *out);

static int g_initialized = 0;
static int g_enabled = 0;
static int g_identity_ok = 0;
static ph_before_send_fn g_before_send = NULL;
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
        return NULL;
    }
    return b.data;
}

static ph_result validate_config(const ph_config *cfg) {
    int i;
    if (!cfg) return PH_ERR_BADARG;
    if (!cfg->enabled) return PH_OK;
    if (!cfg->api_key || !cfg->api_key[0]) return PH_ERR_BADARG;
    if (!cfg->distinct_id || cfg->distinct_id[0] == '\0') return PH_ERR_BADARG;
    if (strlen(cfg->distinct_id) >= PH_DISTINCT_ID_CAP) return PH_ERR_BADARG;
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

    if (g_initialized) return PH_ERR;
    valid = validate_config(cfg);
    if (valid != PH_OK) return valid;

    if (cfg->enabled) {
        /* Verify the host before committing any C-side state. A mismatch is a
         * failed initialization, so the caller can fix the bootstrap and retry. */
        identity_ok = EM_ASM_INT({
            var expected = UTF8ToString($0);
            if (typeof window === 'undefined' || !window.posthog || !expected) return 0;
            return window.__posthog_c_distinct_id === expected ? 1 : 0;
        }, cfg->distinct_id);
        if (!identity_ok) return PH_ERR;
    }

    /* Commit only after every fallible check has passed. */
    g_enabled = cfg->enabled ? 1 : 0;
    g_identity_ok = identity_ok;
    g_before_send = cfg->before_send;
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
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog)
            window.posthog.capture(UTF8ToString($0), JSON.parse(UTF8ToString($1)));
    }, event_capped, json);
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
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog)
            window.posthog.identify(UTF8ToString($0), JSON.parse(UTF8ToString($1)));
    }, id_capped, json);
    free(json);
}

void ph_alias(const char *new_id, const char *old_id) {
    char new_capped[PH_DISTINCT_ID_CAP];
    char old_capped[PH_DISTINCT_ID_CAP];
    ph_props alias_props, clean;
    const char *clean_alias;
    if (!g_enabled || !g_identity_ok || !new_id || !old_id) return;
    ph_copy_capped(new_capped, sizeof(new_capped), new_id);
    ph_copy_capped(old_capped, sizeof(old_capped), old_id);
    ph_props_init(&alias_props);
    ph_props_set_str(&alias_props, "alias", new_capped);
    if (!scrub_props("$create_alias", &alias_props, 0, &clean)) return;
    clean_alias = ph_props_find_last_str(&clean, "alias");
    if (!clean_alias || !clean_alias[0]) return;
    ph_copy_capped(new_capped, sizeof(new_capped), clean_alias);
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog)
            window.posthog.alias(UTF8ToString($0), UTF8ToString($1));
    }, new_capped, old_capped);
}

ph_result ph_get_distinct_id(char *out, int cap) {
    int len;
    if (!out || cap <= 0) return PH_ERR_BADARG;
    out[0] = '\0';
    if (!g_enabled || !g_identity_ok) return PH_ERR_DISABLED;
    len = EM_ASM_INT({
        if (typeof window === 'undefined' || !window.posthog ||
            !window.posthog.get_distinct_id) return -1;
        var id = String(window.posthog.get_distinct_id());
        var len = new TextEncoder().encode(id).length;
        stringToUTF8(id, $0, $1);
        return len;
    }, out, cap);
    if (len < 0) return PH_ERR;
    return len >= cap ? PH_ERR_TRUNCATED : PH_OK;
}

void ph_reset(void) {
    if (!g_enabled) return;
    ph_props_init(&g_super);
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog) {
            window.posthog.reset();
            if (window.posthog.get_distinct_id)
                window.__posthog_c_distinct_id = String(window.posthog.get_distinct_id());
        }
    });
}

void ph_group(const char *type, const char *key, const ph_props *set_props) {
    char *json;
    char type_capped[PH_KEY_CAP];
    char key_capped[PH_KEY_CAP];
    ph_props clean;
    if (!g_enabled || !g_identity_ok || !type || !key) return;
    ph_copy_capped(type_capped, sizeof(type_capped), type);
    ph_copy_capped(key_capped, sizeof(key_capped), key);
    if (!scrub_props("$groupidentify", set_props, 0, &clean)) return;
    json = props_to_json(&clean);
    if (!json) return;
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog)
            window.posthog.group(UTF8ToString($0), UTF8ToString($1),
                                 JSON.parse(UTF8ToString($2)));
    }, type_capped, key_capped, json);
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
        return;
    }

    EM_ASM({
        if (typeof window === 'undefined' || !window.posthog) return;
        var props = JSON.parse(UTF8ToString($0));
        props["$exception_list"] = JSON.parse(UTF8ToString($1));
        window.posthog.capture("$exception", props);
    }, json, list.data);
    ph_strbuf_free(&list);
    free(json);
}

int ph_is_feature_enabled(const char *key, int fallback) {
    if (!g_enabled || !g_identity_ok || !key) return fallback;
    return EM_ASM_INT({
        if (typeof window === 'undefined' || !window.posthog) return $1;
        var key = UTF8ToString($0);
        var v = window.posthog.getFeatureFlag
            ? window.posthog.getFeatureFlag(key)
            : window.posthog.isFeatureEnabled(key);
        if (v === undefined || v === null) return $1;
        return v === false ? 0 : (v ? 1 : 0);
    }, key, fallback);
}

ph_result ph_get_feature_flag(const char *key, char *out, int cap) {
    int found;
    if (out && cap > 0) out[0] = '\0';
    if (!g_enabled || !g_identity_ok || !key) return PH_ERR;
    found = EM_ASM_INT({
        if (typeof window === 'undefined' || !window.posthog) return 0;
        var v = window.posthog.getFeatureFlag(UTF8ToString($0));
        if (v === undefined || v === null) return 0;
        var s = (v === true) ? "true" : (v === false ? "false" : String(v));
        if ($1 && $2 > 0) stringToUTF8(s, $1, $2);
        return 1;
    }, key, out, cap);
    return found ? PH_OK : PH_ERR;
}

ph_result ph_get_feature_flag_payload(const char *key, char *out, int cap) {
    int found;
    if (out && cap > 0) out[0] = '\0';
    if (!g_enabled || !g_identity_ok || !key) return PH_ERR;
    found = EM_ASM_INT({
        if (typeof window === 'undefined' || !window.posthog) return 0;
        var v = window.posthog.getFeatureFlagPayload(UTF8ToString($0));
        if (v === undefined || v === null) return 0;
        var s = JSON.stringify(v);
        if ($1 && $2 > 0) stringToUTF8(s, $1, $2);
        return 1;
    }, key, out, cap);
    return found ? PH_OK : PH_ERR;
}

void ph_reload_feature_flags(void) {
    if (!g_enabled || !g_identity_ok) return;
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog &&
            window.posthog.reloadFeatureFlags)
            window.posthog.reloadFeatureFlags();
    });
}

/* posthog-js owns delivery and its own drop accounting; nothing to report. */
uint64_t ph_dropped_events(void) { return 0; }

/* No SDK-owned queue or thread to drain - posthog-js manages its lifecycle. */
void ph_flush(int timeout_ms) { (void)timeout_ms; }
void ph_shutdown(void) {
    if (g_in_callback) return;
    g_initialized = 0;
    g_enabled = 0;
    g_identity_ok = 0;
    g_before_send = NULL;
    g_user_data = NULL;
    g_denylist_count = 0;
    ph_props_init(&g_super);
    g_fail_next_props_serialize = 0;
}

#endif /* __EMSCRIPTEN__ */
