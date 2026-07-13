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
#include "ph_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* copy_capped lives in ph_util.c (ph_copy_capped), shared across backends. */

/* Request ids remain unique across shutdown/re-init lifecycles. All allocation
 * happens while the current instance's g_ph.lock is held; lifecycle calls are
 * serialized by the public contract. Zero is permanently reserved for UNKNOWN. */
static uint64_t g_reload_request_id;

/* All reload scheduling/history helpers below require g_ph.lock. */
static ph_flag_reload_record *reload_record_locked(uint64_t request_id) {
    int i;
    for (i = 0; i < PH_FLAG_RELOAD_HISTORY_CAP; i++)
        if (g_ph.flag_reload_history[i].request_id == request_id)
            return &g_ph.flag_reload_history[i];
    return NULL;
}

static ph_flag_reload_record *alloc_reload_record_locked(uint64_t request_id,
                                                          uint64_t context_gen) {
    ph_flag_reload_record *slot = NULL;
    int i;
    for (i = 0; i < PH_FLAG_RELOAD_HISTORY_CAP; i++) {
        ph_flag_reload_record *it = &g_ph.flag_reload_history[i];
        if (it->request_id == request_id) return it;
        if (it->request_id == 0) {
            slot = it;
            break;
        }
        if (it->status != PH_FEATURE_FLAG_RELOAD_PENDING &&
            (!slot || it->request_id < slot->request_id))
            slot = it;
    }
    if (!slot) return NULL; /* every fixed slot still has a pending request */
    slot->request_id = request_id;
    slot->context_gen = context_gen;
    slot->status = PH_FEATURE_FLAG_RELOAD_PENDING;
    return slot;
}

static void mark_reload_locked(uint64_t request_id,
                               ph_feature_flag_reload_status status) {
    ph_flag_reload_record *record = reload_record_locked(request_id);
    if (record && record->status == PH_FEATURE_FLAG_RELOAD_PENDING)
        record->status = status;
}

static void mark_context_superseded_locked(uint64_t context_gen) {
    int i;
    for (i = 0; i < PH_FLAG_RELOAD_HISTORY_CAP; i++) {
        ph_flag_reload_record *it = &g_ph.flag_reload_history[i];
        if (it->request_id && it->status == PH_FEATURE_FLAG_RELOAD_PENDING &&
            it->context_gen != context_gen)
            it->status = PH_FEATURE_FLAG_RELOAD_SUPERSEDED;
    }
}

static uint64_t next_reload_request_locked(void) {
    g_reload_request_id++;
    if (g_reload_request_id == 0) g_reload_request_id++;
    return g_reload_request_id;
}

/* Return the queued/in-flight generation for this exact context, or create one.
 * A stale queued job can be discarded without network I/O; an old in-flight job
 * is allowed to finish, but its public token has already been superseded. */
static uint64_t schedule_reload_locked(uint64_t context_gen) {
    uint64_t generation;
    if (g_ph.flags_fetch_inflight &&
        g_ph.flags_fetch_inflight_context_gen == context_gen)
        return g_ph.flags_fetch_inflight_gen;
    if (g_ph.flags_refetch && g_ph.flags_refetch_context_gen == context_gen)
        return g_ph.flags_refetch_gen;

    if (g_ph.flags_refetch) {
        mark_reload_locked(g_ph.flags_refetch_gen,
                           PH_FEATURE_FLAG_RELOAD_SUPERSEDED);
        g_ph.flags_refetch = 0;
        ph_cond_broadcast(&g_ph.flags_cond);
    }
    generation = next_reload_request_locked();
    g_ph.flags_refetch = 1;
    g_ph.flags_refetch_gen = generation;
    g_ph.flags_refetch_context_gen = context_gen;
    return generation;
}

/* A flag value is meaningful only for the exact identity + group context that
 * produced it. Caller holds g_ph.lock, so cache invalidation and public-ticket
 * supersession are observed atomically. */
void ph__flags_context_changed_locked(void) {
    g_ph.flag_count = 0;
    g_ph.flags_context_gen++;
    if (g_ph.flags_context_gen == 0) g_ph.flags_context_gen = 1;
    mark_context_superseded_locked(g_ph.flags_context_gen);
    if (g_ph.flags_refetch &&
        g_ph.flags_refetch_context_gen != g_ph.flags_context_gen) {
        g_ph.flags_refetch = 0;
    }
    ph_cond_broadcast(&g_ph.flags_cond);
}

void ph__flags_request_auto_refresh(void) {
    if (!g_ph.enabled) return;
    ph_mutex_lock(&g_ph.lock);
    (void)schedule_reload_locked(g_ph.flags_context_gen);
    ph_mutex_unlock(&g_ph.lock);
    ph__sender_wake();
}

int ph__flags_take_fetch(uint64_t *generation, uint64_t *context_gen) {
    int have = 0;
    ph_mutex_lock(&g_ph.lock);
    if (g_ph.flags_refetch && !g_ph.flags_fetch_inflight) {
        if (generation) *generation = g_ph.flags_refetch_gen;
        if (context_gen) *context_gen = g_ph.flags_refetch_context_gen;
        g_ph.flags_fetch_inflight = 1;
        g_ph.flags_fetch_inflight_gen = g_ph.flags_refetch_gen;
        g_ph.flags_fetch_inflight_context_gen = g_ph.flags_refetch_context_gen;
        g_ph.flags_refetch = 0;
        have = 1;
    }
    ph_mutex_unlock(&g_ph.lock);
    return have;
}

void ph__flags_complete_fetch(uint64_t generation,
                              ph_feature_flag_reload_status status) {
    ph_mutex_lock(&g_ph.lock);
    if (g_ph.flags_fetch_inflight &&
        g_ph.flags_fetch_inflight_gen == generation) {
        g_ph.flags_fetch_inflight = 0;
        g_ph.flags_fetch_inflight_gen = 0;
        g_ph.flags_fetch_inflight_context_gen = 0;
    }
    mark_reload_locked(generation, status);
    ph_cond_broadcast(&g_ph.flags_cond);
    ph_mutex_unlock(&g_ph.lock);
}

/* Caller holds g_ph.lock. */
static ph_flag *find_locked(const char *key) {
    int i;
    for (i = 0; i < g_ph.flag_count; i++)
        if (strcmp(g_ph.flags[i].key, key) == 0) return &g_ph.flags[i];
    return NULL;
}

static int flag_value_same(const ph_flag *a, const ph_flag *b) {
    if (a->enabled != b->enabled || a->has_variant != b->has_variant) return 0;
    return !a->has_variant || strcmp(a->variant, b->variant) == 0;
}

static int quota_limited(const ph_jv *root) {
    const ph_jv *quota = ph_jv_get(root, "quotaLimited");
    int i;
    if (ph_jv_type_of(quota) != PH_JV_ARR) return 0;
    for (i = 0; i < ph_jv_len(quota); i++) {
        const char *s = ph_jv_str(ph_jv_at(quota, i));
        if (s && strcmp(s, "feature_flags") == 0) return 1;
    }
    return 0;
}

static ph_feature_flag_reload_status failure_for_context(
    uint64_t context_gen) {
    ph_feature_flag_reload_status status = PH_FEATURE_FLAG_RELOAD_FAILED;
    ph_mutex_lock(&g_ph.lock);
    if (context_gen != g_ph.flags_context_gen)
        status = PH_FEATURE_FLAG_RELOAD_SUPERSEDED;
    ph_mutex_unlock(&g_ph.lock);
    return status;
}

/* Apply a response only to the identity/group generation that requested it.
 * A slower, older request must never overwrite a newer user's cache. */
static ph_feature_flag_reload_status flags_ingest_for_context(
    const char *json, size_t len, uint64_t context_gen) {
    ph_jv *root = ph_jv_parse(json, len);
    const ph_jv *flags;
    ph_flag *parsed; /* PH_MAX_FLAGS entries on the heap, not ~40 KB of sender
                      * stack; embedders may configure a small worker stack. */
    int parsed_count = 0;
    int partial, i;
    if (!root) return failure_for_context(context_gen);
    if (quota_limited(root)) {
        ph_log(PH_LOG_WARN, "flags: quota limited; retaining same-context cache");
        ph_jv_free(root);
        return failure_for_context(context_gen);
    }
    flags = ph_jv_get(root, "flags");
    if (ph_jv_type_of(flags) != PH_JV_OBJ) {
        ph_jv_free(root);
        return failure_for_context(context_gen);
    }
    parsed = malloc(sizeof(*parsed) * PH_MAX_FLAGS);
    if (!parsed) {
        ph_jv_free(root);
        return failure_for_context(context_gen);
    }

    partial = ph_jv_bool(ph_jv_get(root, "errorsWhileComputingFlags"));
    for (i = 0; i < ph_jv_len(flags) && parsed_count < PH_MAX_FLAGS; i++) {
        const char *key = ph_jv_key_at(flags, i);
        const ph_jv *f = ph_jv_val_at(flags, i);
        const ph_jv *variant = ph_jv_get(f, "variant");
        const ph_jv *meta = ph_jv_get(f, "metadata");
        const ph_jv *payload = meta ? ph_jv_get(meta, "payload") : NULL;
        ph_flag *slot = &parsed[parsed_count];
        if (!key) continue;
        memset(slot, 0, sizeof(*slot));
        ph_copy_capped(slot->key, PH_KEY_CAP, key);
        slot->enabled = ph_jv_bool(ph_jv_get(f, "enabled"));
        if (ph_jv_type_of(variant) == PH_JV_STR) {
            slot->has_variant = 1;
            ph_copy_capped(slot->variant, PH_FLAG_VARIANT_CAP, ph_jv_str(variant));
        }
        if (ph_jv_type_of(payload) == PH_JV_STR) {
            slot->has_payload = 1;
            ph_copy_capped(slot->payload, PH_FLAG_PAYLOAD_CAP, ph_jv_str(payload));
        }
        parsed_count++;
    }

    ph_mutex_lock(&g_ph.lock);
    if (context_gen != g_ph.flags_context_gen) {
        ph_mutex_unlock(&g_ph.lock);
        free(parsed);
        ph_jv_free(root);
        return PH_FEATURE_FLAG_RELOAD_SUPERSEDED;
    }

    if (!partial) {
        /* Preserve the $feature_flag_called latch only where the value is
         * unchanged. Decide that first (find_locked reads the live cache), then
         * overwrite in place - no second PH_MAX_FLAGS-sized stack copy. */
        unsigned char keep_called[PH_MAX_FLAGS];
        for (i = 0; i < parsed_count; i++) {
            ph_flag *old = find_locked(parsed[i].key);
            keep_called[i] = (old && old->called_sent &&
                              flag_value_same(old, &parsed[i]))
                                 ? 1
                                 : 0;
        }
        for (i = 0; i < parsed_count; i++) {
            g_ph.flags[i] = parsed[i];
            g_ph.flags[i].called_sent = keep_called[i];
        }
        g_ph.flag_count = parsed_count;
    } else {
        /* PostHog marks partial responses so SDKs retain flags that were not
         * recomputed. Replace only the entries explicitly returned. */
        for (i = 0; i < parsed_count; i++) {
            ph_flag *old = find_locked(parsed[i].key);
            int called = old && old->called_sent && flag_value_same(old, &parsed[i]);
            if (!old) {
                if (g_ph.flag_count >= PH_MAX_FLAGS) continue;
                old = &g_ph.flags[g_ph.flag_count++];
            }
            *old = parsed[i];
            old->called_sent = called;
        }
    }
    ph_mutex_unlock(&g_ph.lock);
    free(parsed);
    ph_jv_free(root);
    return partial ? PH_FEATURE_FLAG_RELOAD_FAILED
                   : PH_FEATURE_FLAG_RELOAD_SUCCESS;
}

ph_feature_flag_reload_status ph__flags_ingest(const char *json, size_t len) {
    uint64_t context_gen;
    ph_mutex_lock(&g_ph.lock);
    context_gen = g_ph.flags_context_gen;
    ph_mutex_unlock(&g_ph.lock);
    return flags_ingest_for_context(json, len, context_gen);
}

static void flags_url(char *out, size_t cap) {
    size_t n = strlen(g_ph.api_host);
    while (n > 0 && g_ph.api_host[n - 1] == '/') n--;
    snprintf(out, cap, "%.*s/flags?v=2", (int)n, g_ph.api_host);
}

ph_feature_flag_reload_status ph__flags_fetch(uint64_t expected_context_gen) {
    ph_strbuf body;
    ph_transport t;
    char url[PH_HOST_CAP + 16];
    char *resp;
    uint64_t context_gen;
    int status, i;

    /* Build {"api_key","distinct_id","groups":{...}}. Snapshot identity under
     * the lock; ph__flags_ingest re-locks later, so release it before fetching. */
    ph_strbuf_init(&body);
    ph_strbuf_append_cstr(&body, "{\"api_key\":");
    ph_json_cstr(&body, g_ph.api_key);
    if (g_ph.disable_geoip)
        ph_strbuf_append_cstr(&body, ",\"geoip_disable\":true");
    ph_mutex_lock(&g_ph.lock);
    if (expected_context_gen != g_ph.flags_context_gen) {
        ph_mutex_unlock(&g_ph.lock);
        ph_strbuf_free(&body);
        return PH_FEATURE_FLAG_RELOAD_SUPERSEDED;
    }
    context_gen = g_ph.flags_context_gen;
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
    if (g_ph.flag_person_props.count > 0) {
        ph_strbuf_append_cstr(&body, ",\"person_properties\":");
        ph_serialize_props_object(&g_ph.flag_person_props, &body);
    }
    {
        int have_group_props = 0;
        for (i = 0; i < g_ph.group_count; i++) {
            if (g_ph.group_props[i].count <= 0) continue;
            if (!have_group_props) {
                ph_strbuf_append_cstr(&body, ",\"group_properties\":{");
                have_group_props = 1;
            } else {
                ph_strbuf_append_char(&body, ',');
            }
            ph_json_cstr(&body, g_ph.group_types[i]);
            ph_strbuf_append_char(&body, ':');
            ph_serialize_props_object(&g_ph.group_props[i], &body);
        }
        if (have_group_props) ph_strbuf_append_char(&body, '}');
    }
    ph_mutex_unlock(&g_ph.lock);
    ph_strbuf_append_char(&body, '}');

    if (body.oom) {
        ph_strbuf_free(&body);
        return failure_for_context(context_gen);
    }

    flags_url(url, sizeof(url));

    ph_mutex_lock(&g_ph.flush_lock);
    t = g_ph.transport;
    ph_mutex_unlock(&g_ph.flush_lock);

    if (!t.fetch) {
        ph_strbuf_free(&body);
        ph_log(PH_LOG_WARN, "flags: transport has no fetch (https needs TLS on this platform)");
        return failure_for_context(context_gen);
    }

    resp = (char *)malloc(PH_FLAGS_RESP_CAP);
    if (!resp) {
        ph_strbuf_free(&body);
        return failure_for_context(context_gen);
    }
    status = t.fetch(t.self, url, body.data ? body.data : "{}", body.len,
                     g_ph.request_timeout_ms, resp, PH_FLAGS_RESP_CAP);
    if (status >= 200 && status < 300) {
        ph_feature_flag_reload_status outcome =
            flags_ingest_for_context(resp, strlen(resp), context_gen);
        free(resp);
        ph_strbuf_free(&body);
        return outcome;
    } else {
        ph_log(PH_LOG_WARN, "flags fetch to %s failed (status %d)", url, status);
    }

    free(resp);
    ph_strbuf_free(&body);
    return failure_for_context(context_gen);
}

/* --- accessors + public flag API -------------------------------------- */

/* Deduped exposure event, emitted the first time a flag value is read. */
static void emit_ff_called(const char *key, const char *value, int has_variant,
                           int enabled) {
    ph_props p;
    if (!g_ph.enabled) return;
    ph_props_init(&p);
    ph_props_set_str(&p, "$feature_flag", key);
    if (has_variant)
        ph_props_set_str(&p, "$feature_flag_response", value);
    else
        ph_props_set_bool(&p, "$feature_flag_response", enabled);
    ph__submit_event(PH_EV_CAPTURE, 0, "$feature_flag_called", NULL, &p, -1, 1, NULL, 0);
}

/* Resolve a flag's "value" string (variant, or "true"/"false"), remember it as
 * read for $feature_flag_called dedup, and report whether the flag was cached.
 * Returns 1 if found (filling `value`), 0 otherwise. Sets *emit when a
 * (first-time) exposure event should be sent. */
static int resolve(const char *key, char *value, size_t vcap, int *enabled,
                    int *has_variant, int *emit) {
    ph_flag *f;
    int found = 0;
    *emit = 0;
    ph_mutex_lock(&g_ph.lock);
    f = find_locked(key);
    if (f) {
        found = 1;
        if (enabled) *enabled = f->enabled;
        if (has_variant) *has_variant = f->has_variant;
        ph_copy_capped(value, vcap, f->has_variant ? f->variant : (f->enabled ? "true" : "false"));
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
    int enabled = fallback, has_variant = 0, emit = 0;
    int found;
    if (!key) return fallback;
    found = resolve(key, value, sizeof(value), &enabled, &has_variant, &emit);
    if (emit) emit_ff_called(key, value, has_variant, enabled);
    return found ? enabled : fallback;
}

ph_result ph__flags_get(const char *key, char *out, int cap) {
    char value[PH_FLAG_VARIANT_CAP];
    int enabled = 0, has_variant = 0, emit = 0, found;
    if (out && cap > 0) out[0] = '\0';
    if (!key) return PH_ERR;
    found = resolve(key, value, sizeof(value), &enabled, &has_variant, &emit);
    if (found && out && cap > 0) ph_copy_capped(out, (size_t)cap, value);
    if (emit) emit_ff_called(key, value, has_variant, enabled);
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
        if (out && cap > 0) ph_copy_capped(out, (size_t)cap, f->payload);
    }
    ph_mutex_unlock(&g_ph.lock);
    return found ? PH_OK : PH_ERR;
}

/* --- public flag API (thin wrappers over the internal accessors) ------- */

int ph_is_feature_enabled(const char *key, int fallback) {
    if (!g_ph.enabled) return fallback;
    return ph__flags_is_enabled(key, fallback);
}

ph_result ph_get_feature_flag(const char *key, char *out, int cap) {
    if (!g_ph.enabled) {
        if (out && cap > 0) out[0] = '\0';
        return PH_ERR;
    }
    return ph__flags_get(key, out, cap);
}

ph_result ph_get_feature_flag_payload(const char *key, char *out, int cap) {
    if (!g_ph.enabled) {
        if (out && cap > 0) out[0] = '\0';
        return PH_ERR;
    }
    return ph__flags_get_payload(key, out, cap);
}

ph_result ph_reload_feature_flags_async(uint64_t *request_id) {
    ph_flag_reload_record *record;
    uint64_t generation;
    if (!request_id) return PH_ERR_BADARG;
    *request_id = 0;
    if (!g_ph.enabled || !g_ph.sender_running) return PH_ERR_DISABLED;

    ph_mutex_lock(&g_ph.lock);
    generation = schedule_reload_locked(g_ph.flags_context_gen);
    record = alloc_reload_record_locked(generation, g_ph.flags_context_gen);
    if (record) *request_id = generation;
    ph_mutex_unlock(&g_ph.lock);
    ph__sender_wake();
    return record ? PH_OK : PH_ERR_FULL;
}

ph_feature_flag_reload_status ph_get_feature_flag_reload_status(
    uint64_t request_id) {
    ph_flag_reload_record *record;
    ph_feature_flag_reload_status status = PH_FEATURE_FLAG_RELOAD_UNKNOWN;
    if (!request_id || !g_ph.enabled) return status;
    ph_mutex_lock(&g_ph.lock);
    record = reload_record_locked(request_id);
    if (record) status = record->status;
    ph_mutex_unlock(&g_ph.lock);
    return status;
}

void ph_reload_feature_flags(void) {
    ph_feature_flag_reload_status status;
    uint64_t request_id;
    for (;;) {
        if (ph_reload_feature_flags_async(&request_id) != PH_OK) return;
        /* A sender callback can safely attach to the current job, but waiting
         * for that same sender to complete it would deadlock. */
        if (ph__in_callback || ph_thread_is_current(&g_ph.sender)) return;

        ph_mutex_lock(&g_ph.lock);
        for (;;) {
            ph_flag_reload_record *record = reload_record_locked(request_id);
            status = record ? record->status : PH_FEATURE_FLAG_RELOAD_UNKNOWN;
            if (status != PH_FEATURE_FLAG_RELOAD_PENDING) break;
            ph_cond_timedwait(&g_ph.flags_cond, &g_ph.lock, 100);
        }
        ph_mutex_unlock(&g_ph.lock);
        if (status != PH_FEATURE_FLAG_RELOAD_SUPERSEDED) return;
        /* Identity/group inputs changed while this request was pending. Queue
         * and wait for the successor context so the legacy void API remains a
         * true blocking refresh for ordinary callers. */
    }
}
