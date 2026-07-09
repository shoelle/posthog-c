/*
 * ph_flags.c - remote feature-flag evaluation + cache.
 *
 * A client SDK can't hold the personal key server SDKs use for local eval, so
 * flags are evaluated remotely: POST /flags?v=2 with the distinct_id, cache the
 * result, and answer ph_is_feature_enabled / ph_get_feature_flag(_payload) off
 * the cache. Reading a flag emits a deduped $feature_flag_called so experiments
 * can measure exposure.
 */
#include "ph_internal.h"
#include "ph_json.h"
#include "ph_jsonval.h"
#include "ph_str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_capped(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (cap == 0) return;
    if (src)
        for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* Caller holds g_ph.lock. */
static ph_flag *find_locked(const char *key) {
    int i;
    for (i = 0; i < g_ph.flag_count; i++)
        if (strcmp(g_ph.flags[i].key, key) == 0) return &g_ph.flags[i];
    return NULL;
}

void ph__flags_ingest(const char *json, size_t len) {
    ph_jv *root = ph_jv_parse(json, len);
    const ph_jv *flags;
    int i;
    if (!root) return;
    flags = ph_jv_get(root, "flags");
    if (ph_jv_type_of(flags) != PH_JV_OBJ) {
        ph_jv_free(root);
        return;
    }

    ph_mutex_lock(&g_ph.lock);
    g_ph.flag_count = 0;
    for (i = 0; i < ph_jv_len(flags) && g_ph.flag_count < PH_MAX_FLAGS; i++) {
        const char *key = ph_jv_key_at(flags, i);
        const ph_jv *f = ph_jv_val_at(flags, i);
        const ph_jv *variant = ph_jv_get(f, "variant");
        const ph_jv *meta = ph_jv_get(f, "metadata");
        const ph_jv *payload = meta ? ph_jv_get(meta, "payload") : NULL;
        ph_flag *slot = &g_ph.flags[g_ph.flag_count];
        if (!key) continue;
        memset(slot, 0, sizeof(*slot));
        copy_capped(slot->key, PH_KEY_CAP, key);
        slot->enabled = ph_jv_bool(ph_jv_get(f, "enabled"));
        if (ph_jv_type_of(variant) == PH_JV_STR) {
            slot->has_variant = 1;
            copy_capped(slot->variant, PH_FLAG_VARIANT_CAP, ph_jv_str(variant));
        }
        if (ph_jv_type_of(payload) == PH_JV_STR) {
            slot->has_payload = 1;
            copy_capped(slot->payload, PH_FLAG_PAYLOAD_CAP, ph_jv_str(payload));
        }
        g_ph.flag_count++;
    }
    ph_mutex_unlock(&g_ph.lock);
    ph_jv_free(root);
}

static void flags_url(char *out, size_t cap) {
    size_t n = strlen(g_ph.api_host);
    while (n > 0 && g_ph.api_host[n - 1] == '/') n--;
    snprintf(out, cap, "%.*s/flags?v=2", (int)n, g_ph.api_host);
}

void ph__flags_fetch(void) {
    ph_strbuf body;
    ph_transport t;
    char url[PH_HOST_CAP + 16];
    char *resp;
    int status, i;

    /* Build {"api_key","distinct_id","groups":{...}}. Snapshot identity under
     * the lock; ph__flags_ingest re-locks later, so release it before fetching. */
    ph_strbuf_init(&body);
    ph_strbuf_append_cstr(&body, "{\"api_key\":");
    ph_json_cstr(&body, g_ph.api_key);
    ph_mutex_lock(&g_ph.lock);
    ph_strbuf_append_cstr(&body, ",\"distinct_id\":");
    ph_json_cstr(&body, g_ph.distinct_id);
    if (g_ph.group_count > 0) {
        ph_strbuf_append_cstr(&body, ",\"groups\":{");
        for (i = 0; i < g_ph.group_count; i++) {
            if (i > 0) ph_strbuf_append_char(&body, ',');
            ph_json_cstr(&body, g_ph.group_types[i]);
            ph_strbuf_append_char(&body, ':');
            ph_json_cstr(&body, g_ph.group_keys[i]);
        }
        ph_strbuf_append_char(&body, '}');
    }
    ph_mutex_unlock(&g_ph.lock);
    ph_strbuf_append_char(&body, '}');

    if (body.oom) {
        ph_strbuf_free(&body);
        return;
    }

    flags_url(url, sizeof(url));

    ph_mutex_lock(&g_ph.flush_lock);
    t = g_ph.transport;
    ph_mutex_unlock(&g_ph.flush_lock);

    if (!t.fetch) {
        ph_strbuf_free(&body);
        ph_log(PH_LOG_WARN, "flags: transport has no fetch (https needs TLS on this platform)");
        return;
    }

    resp = (char *)malloc(PH_FLAGS_RESP_CAP);
    if (!resp) {
        ph_strbuf_free(&body);
        return;
    }
    status = t.fetch(t.self, url, body.data ? body.data : "{}", body.len,
                     g_ph.request_timeout_ms, resp, PH_FLAGS_RESP_CAP);
    if (status >= 200 && status < 300)
        ph__flags_ingest(resp, strlen(resp));
    else
        ph_log(PH_LOG_WARN, "flags fetch to %s failed (status %d)", url, status);

    free(resp);
    ph_strbuf_free(&body);
}

/* --- accessors -------------------------------------------------------- */

/* Resolve a flag's "value" string (variant, or "true"/"false"), remember it as
 * read for $feature_flag_called dedup, and report whether the flag was cached.
 * Returns 1 if found (filling `value`), 0 otherwise. Sets *emit when a
 * (first-time) exposure event should be sent. */
static int resolve(const char *key, char *value, size_t vcap, int *enabled,
                   int *emit) {
    ph_flag *f;
    int found = 0;
    *emit = 0;
    ph_mutex_lock(&g_ph.lock);
    f = find_locked(key);
    if (f) {
        found = 1;
        if (enabled) *enabled = f->enabled;
        copy_capped(value, vcap, f->has_variant ? f->variant : (f->enabled ? "true" : "false"));
        if (g_ph.send_feature_flag_events && !f->called_sent) {
            f->called_sent = 1;
            *emit = 1;
        }
    }
    ph_mutex_unlock(&g_ph.lock);
    return found;
}

int ph__flags_is_enabled(const char *key, int fallback) {
    char value[PH_FLAG_VARIANT_CAP];
    int enabled = fallback, emit = 0;
    int found;
    if (!key) return fallback;
    found = resolve(key, value, sizeof(value), &enabled, &emit);
    if (emit) ph__emit_ff_called(key, value);
    return found ? enabled : fallback;
}

ph_result ph__flags_get(const char *key, char *out, int cap) {
    char value[PH_FLAG_VARIANT_CAP];
    int emit = 0, found;
    if (out && cap > 0) out[0] = '\0';
    if (!key) return PH_ERR;
    found = resolve(key, value, sizeof(value), NULL, &emit);
    if (found && out && cap > 0) copy_capped(out, (size_t)cap, value);
    if (emit) ph__emit_ff_called(key, value);
    return found ? PH_OK : PH_ERR;
}

ph_result ph__flags_get_payload(const char *key, char *out, int cap) {
    ph_flag *f;
    int found = 0;
    if (out && cap > 0) out[0] = '\0';
    if (!key) return PH_ERR;
    ph_mutex_lock(&g_ph.lock);
    f = find_locked(key);
    if (f && f->has_payload) {
        found = 1;
        if (out && cap > 0) copy_capped(out, (size_t)cap, f->payload);
    }
    ph_mutex_unlock(&g_ph.lock);
    return found ? PH_OK : PH_ERR;
}
