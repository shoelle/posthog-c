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

const char *ph_version(void) { return PH_VERSION_STRING; }

static void copy_capped(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (cap == 0) return;
    if (src)
        for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void copy_prop_value(ph_props *dst, const ph_prop *src) {
    switch (src->type) {
        case PH_T_STR: ph_props_set_str(dst, src->key, src->val.str); break;
        case PH_T_DOUBLE: ph_props_set_double(dst, src->key, src->val.dbl); break;
        case PH_T_INT: ph_props_set_int(dst, src->key, src->val.i64); break;
        case PH_T_BOOL: ph_props_set_bool(dst, src->key, src->val.boolean); break;
        default: break;
    }
}

static void remove_key(ph_props *p, const char *key) {
    int i, k;
    for (i = 0; p && i < p->count;) {
        if (strcmp(p->items[i].key, key) == 0) {
            for (k = i; k + 1 < p->count; k++) p->items[k] = p->items[k + 1];
            p->count--;
        } else {
            i++;
        }
    }
}

static const char *find_last_str(const ph_props *p, const char *key) {
    int i;
    if (!p || !key) return NULL;
    for (i = p->count - 1; i >= 0; i--) {
        const ph_prop *it = &p->items[i];
        if (it->type == PH_T_STR && strcmp(it->key, key) == 0) return it->val.str;
    }
    return NULL;
}

static void apply_denylist(ph_props *p) {
    int i;
    for (i = 0; i < g_denylist_count; i++) remove_key(p, g_denylist[i]);
}

static int denylist_has(const char *key) {
    int i;
    for (i = 0; key && i < g_denylist_count; i++)
        if (strcmp(g_denylist[i], key) == 0) return 1;
    return 0;
}

static int scrub_props(const char *event, const ph_props *in, ph_props *out) {
    int i;
    ph_props_init(out);
    for (i = 0; i < g_super.count; i++) copy_prop_value(out, &g_super.items[i]);
    if (in) {
        for (i = 0; i < in->count; i++) copy_prop_value(out, &in->items[i]);
    }
    apply_denylist(out);
    if (g_before_send && !g_before_send(event, out, g_user_data)) return 0;
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
                copy_capped(g_denylist[g_denylist_count++], PH_KEY_CAP,
                            cfg->property_denylist[i]);
        }
    }

    if (!g_enabled) {
        g_identity_ok = 0;
        return PH_OK;
    }
    if (!cfg->distinct_id || cfg->distinct_id[0] == '\0') return PH_ERR_BADARG;

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

void ph_capture(const char *event, const ph_props *props) {
    char *json;
    ph_props clean;
    if (!g_enabled || !g_identity_ok || !event || !event[0]) return;
    if (!scrub_props(event, props, &clean)) return;
    json = props_to_json(&clean);
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog)
            window.posthog.capture(UTF8ToString($0), JSON.parse(UTF8ToString($1)));
    }, event, json ? json : "{}");
    free(json);
}

void ph_identify(const char *distinct_id, const ph_props *set_props) {
    char *json;
    if (!g_enabled || !g_identity_ok || !distinct_id || !distinct_id[0]) return;
    json = props_to_json(set_props);
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog)
            window.posthog.identify(UTF8ToString($0), JSON.parse(UTF8ToString($1)));
    }, distinct_id, json ? json : "{}");
    free(json);
}

void ph_alias(const char *new_id, const char *old_id) {
    if (!g_enabled || !g_identity_ok || !new_id || !old_id) return;
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog)
            window.posthog.alias(UTF8ToString($0), UTF8ToString($1));
    }, new_id, old_id);
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
    if (!g_enabled || !g_identity_ok || !type || !key) return;
    json = props_to_json(set_props);
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog)
            window.posthog.group(UTF8ToString($0), UTF8ToString($1),
                                 JSON.parse(UTF8ToString($2)));
    }, type, key, json ? json : "{}");
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
    remove_key(&g_super, key);
}

void ph_capture_exception(const ph_exception *ex) {
    char *json;
    ph_props clean;
    char type[PH_VAL_CAP];
    char message[PH_VAL_CAP];
    const char *v;
    if (!g_enabled || !g_identity_ok || !ex) return;
    ph_props_init(&clean);
    {
        int i;
        for (i = 0; i < g_super.count; i++) copy_prop_value(&clean, &g_super.items[i]);
    }
    ph_props_set_str(&clean, "$exception_type", ex->type ? ex->type : "Error");
    ph_props_set_str(&clean, "$exception_message", ex->message ? ex->message : "");
    if (ex->extra) {
        int i;
        for (i = 0; i < ex->extra->count; i++) copy_prop_value(&clean, &ex->extra->items[i]);
    }
    apply_denylist(&clean);
    if (denylist_has("type")) remove_key(&clean, "$exception_type");
    if (denylist_has("message")) remove_key(&clean, "$exception_message");
    if (g_before_send && !g_before_send("$exception", &clean, g_user_data)) return;
    v = find_last_str(&clean, "$exception_type");
    copy_capped(type, sizeof(type), v ? v : "Error");
    v = find_last_str(&clean, "$exception_message");
    copy_capped(message, sizeof(message), v ? v : "");
    remove_key(&clean, "$exception_type");
    remove_key(&clean, "$exception_message");
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
void ph_shutdown(void) { g_enabled = 0; g_identity_ok = 0; }

#endif /* __EMSCRIPTEN__ */
