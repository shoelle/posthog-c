/*
 * ph_wasm.c — the WebAssembly backend: a thin shim over the browser's already
 * loaded posthog-js (§3, §7). Selected at compile time by __EMSCRIPTEN__.
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
 * module unchanged. ph_flush/ph_shutdown are no-ops — posthog-js manages its
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

const char *ph_version(void) { return PH_VERSION_STRING; }

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
    cfg->gzip = 1;
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
    g_enabled = cfg && cfg->enabled;
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
    if (!g_enabled || !g_identity_ok || !event || !event[0]) return;
    json = props_to_json(props);
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
    char *json;
    if (!g_enabled || !g_identity_ok || !super_props) return;
    json = props_to_json(super_props);
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog)
            window.posthog.register(JSON.parse(UTF8ToString($0)));
    }, json ? json : "{}");
    free(json);
}

void ph_unregister(const char *key) {
    if (!g_enabled || !g_identity_ok || !key) return;
    EM_ASM({
        if (typeof window !== 'undefined' && window.posthog)
            window.posthog.unregister(UTF8ToString($0));
    }, key);
}

void ph_capture_exception(const ph_exception *ex) {
    char *json;
    if (!g_enabled || !g_identity_ok || !ex) return;
    json = props_to_json(ex->extra);
    EM_ASM({
        if (typeof window === 'undefined' || !window.posthog) return;
        var err = new Error(UTF8ToString($1));
        err.name = UTF8ToString($0);
        var extra = JSON.parse(UTF8ToString($2));
        extra.$exception_handled = $3 ? true : false;
        window.posthog.captureException(err, extra);
    }, ex->type ? ex->type : "Error", ex->message ? ex->message : "", json ? json : "{}",
       ex->handled);
    free(json);
}

int ph_is_feature_enabled(const char *key, int fallback) {
    if (!g_enabled || !g_identity_ok || !key) return fallback;
    return EM_ASM_INT({
        if (typeof window === 'undefined' || !window.posthog) return $1;
        return window.posthog.isFeatureEnabled(UTF8ToString($0)) ? 1 : 0;
    }, key, fallback);
}

ph_result ph_get_feature_flag(const char *key, char *out, int cap) {
    int found;
    if (out && cap > 0) out[0] = '\0';
    if (!g_enabled || !g_identity_ok || !key) return PH_ERR;
    found = EM_ASM_INT({
        if (typeof window === 'undefined' || !window.posthog) return 0;
        var v = window.posthog.getFeatureFlag(UTF8ToString($0));
        if (v === undefined || v === null || v === false) return 0;
        var s = (v === true) ? "true" : String(v);
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

/* No SDK-owned queue or thread to drain — posthog-js manages its lifecycle. */
void ph_flush(int timeout_ms) { (void)timeout_ms; }
void ph_shutdown(void) { g_enabled = 0; g_identity_ok = 0; }

#endif /* __EMSCRIPTEN__ */
