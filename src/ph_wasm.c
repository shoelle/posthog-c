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
 * property shapes stay comparable across backends.
 *
 * The public API is implemented entirely here; the shared property/JSON code
 * (ph_props.c, ph_json.c, ph_str.c, ph_serialize.c) compiles into the wasm
 * module unchanged. ph_flush/ph_shutdown are no-ops - posthog-js manages its
 * own lifecycle.
 */
#if defined(__EMSCRIPTEN__)

#include "posthog.h"
#include "ph_str.h"
#include "ph_util.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

/* Defined in ph_serialize.c (shared). Forward-declared so this TU doesn't pull
 * in the native-only internal header. */
void ph_serialize_props_object(const ph_props *p, ph_strbuf *out);

static int g_enabled = 0;
static int g_identity_ok = 0;
static ph_before_send_fn g_before_send = NULL;
static void *g_user_data = NULL;
static char g_denylist[PH_MAX_DENYLIST][PH_KEY_CAP];
static int g_denylist_count = 0;
static ph_props g_super;
static _Thread_local int g_in_callback;

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

/* Serialize props to a malloc'd JSON object string (caller frees). "{}" when
 * empty or on allocation failure. */
static char *props_to_json(const ph_props *p) {
    ph_strbuf b;
    ph_strbuf_init(&b);
    if (p)
        ph_serialize_props_object(p, &b);
    else
        ph_strbuf_append_cstr(&b, "{}");
    if (!b.data) {
        char *fallback = (char *)malloc(3);
        if (fallback) {
            fallback[0] = '{';
            fallback[1] = '}';
            fallback[2] = '\0';
        }
        return fallback;
    }
    return b.data;
}

ph_result ph_init(const ph_config *cfg) {
    int i, n;
    if (!cfg) return PH_ERR_BADARG;

    g_enabled = cfg->enabled;
    g_before_send = cfg->before_send;
    g_user_data = cfg->user_data;
    g_denylist_count = 0;
    ph_props_init(&g_super);
    if (cfg->property_denylist && cfg->property_denylist_count > 0) {
        n = cfg->property_denylist_count;
        if (n > PH_MAX_DENYLIST) n = PH_MAX_DENYLIST;
        for (i = 0; i < n; i++) {
            if (cfg->property_denylist[i] && cfg->property_denylist[i][0])
                ph_copy_capped(g_denylist[g_denylist_count++], PH_KEY_CAP,
                            cfg->property_denylist[i]);
        }
    }

    if (!g_enabled) {
        g_identity_ok = 0;
        return PH_OK;
    }
    if (!cfg->api_key || !cfg->api_key[0]) return PH_ERR_BADARG;
    if (!cfg->distinct_id || cfg->distinct_id[0] == '\0') return PH_ERR_BADARG;
    if (strlen(cfg->distinct_id) >= PH_DISTINCT_ID_CAP) return PH_ERR_BADARG;

    /* Verify the host bootstrapped posthog-js with our install id. We read the
     * host-owned variable synchronously rather than posthog.get_distinct_id(),
     * which is only reliable after posthog-js's async load completes. */
    g_identity_ok = EM_ASM_INT({
        var expected = UTF8ToString($0);
        if (typeof window === 'undefined' || !window.posthog || !expected) return 0;
        return window.__posthog_c_distinct_id === expected ? 1 : 0;
    }, cfg->distinct_id);

    return (g_enabled && g_identity_ok) ? PH_OK : PH_ERR;
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
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog)
            window.posthog.capture(UTF8ToString($0), JSON.parse(UTF8ToString($1)));
    }, event_capped, json ? json : "{}");
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
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog)
            window.posthog.identify(UTF8ToString($0), JSON.parse(UTF8ToString($1)));
    }, id_capped, json ? json : "{}");
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

void ph_reset(void) {
    if (!g_enabled) return;
    ph_props_init(&g_super);
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog) window.posthog.reset();
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
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog)
            window.posthog.group(UTF8ToString($0), UTF8ToString($1),
                                 JSON.parse(UTF8ToString($2)));
    }, type_capped, key_capped, json ? json : "{}");
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

void ph_capture_exception(const ph_exception *ex) {
    char *json;
    ph_props base, clean;
    char type[PH_VAL_CAP];
    char message[PH_VAL_CAP];
    const char *v;
    if (!g_enabled || !g_identity_ok || !ex) return;
    ph_props_init(&base);
    ph_props_set_str(&base, "$exception_type", ex->type ? ex->type : "Error");
    ph_props_set_str(&base, "$exception_message", ex->message ? ex->message : "");
    if (ex->extra) {
        int i;
        for (i = 0; i < ex->extra->count; i++) ph_copy_prop_value(&base, &ex->extra->items[i]);
    }
    ph_props_merge(&clean, &base, &g_super);
    apply_denylist(&clean);
    if (denylist_has("type")) ph_props_remove_key(&clean, "$exception_type");
    if (denylist_has("message")) ph_props_remove_key(&clean, "$exception_message");
    if (g_before_send) {
        int keep;
        g_in_callback++;
        keep = g_before_send("$exception", &clean, g_user_data);
        g_in_callback--;
        if (!keep) return;
    }
    v = ph_props_find_last_str(&clean, "$exception_type");
    ph_copy_capped(type, sizeof(type), v ? v : "Error");
    v = ph_props_find_last_str(&clean, "$exception_message");
    ph_copy_capped(message, sizeof(message), v ? v : "");
    ph_props_remove_key(&clean, "$exception_type");
    ph_props_remove_key(&clean, "$exception_message");
    json = props_to_json(&clean);
    EM_ASM({
        if (typeof window === 'undefined' || !window.posthog) return;
        var err = new Error(UTF8ToString($1));
        err.name = UTF8ToString($0);
        var extra = JSON.parse(UTF8ToString($2));
        extra.$exception_handled = $3 ? true : false;
        window.posthog.captureException(err, extra);
    }, type, message, json ? json : "{}", ex->handled);
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
    g_enabled = 0;
    g_identity_ok = 0;
}

#endif /* __EMSCRIPTEN__ */
