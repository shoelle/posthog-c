/*
 * ph_core.c - the public API surface and the shared enqueue path.
 *
 * Every public call funnels through submit_event(), which snapshots identity /
 * super-properties / group scoping under one mutex and packs a self-contained
 * event into the ring. Because each event bakes its own distinct_id and profile
 * decision at capture time, the sender needs no shared identity state, and the
 * ordering "events captured before ph_identify keep the anonymous id" falls out
 * for free.
 */
#include "ph_internal.h"
#include "ph_crash.h"
#include "ph_http.h"
#include "ph_json.h"
#include "ph_str.h"
#include "ph_time.h"
#include "ph_tls.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ph_ctx g_ph;

const char *ph_version(void) { return PH_VERSION_STRING; }

/* --- small helpers ---------------------------------------------------- */

static void copy_str(char *dst, size_t cap, const char *src) {
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

static void remove_prop_key(ph_props *p, const char *key) {
    int i, k;
    if (!p || !key) return;
    for (i = 0; i < p->count;) {
        if (strcmp(p->items[i].key, key) == 0) {
            for (k = i; k + 1 < p->count; k++) p->items[k] = p->items[k + 1];
            p->count--;
        } else {
            i++;
        }
    }
}

static const char *find_last_str_prop(const ph_props *p, const char *key) {
    int i;
    if (!p || !key) return NULL;
    for (i = p->count - 1; i >= 0; i--) {
        const ph_prop *it = &p->items[i];
        if (it->type == PH_T_STR && strcmp(it->key, key) == 0)
            return it->val.str;
    }
    return NULL;
}

static int denylist_has(const char *key) {
    int i;
    if (!key) return 0;
    for (i = 0; i < g_ph.denylist_count; i++)
        if (strcmp(g_ph.denylist[i], key) == 0) return 1;
    return 0;
}

static void apply_denylist(ph_props *p) {
    int i;
    for (i = 0; i < g_ph.denylist_count; i++) remove_prop_key(p, g_ph.denylist[i]);
}

static int def_int(int v, int fallback) { return v > 0 ? v : fallback; }

static void gen_anon_id(char *out) {
    char uuid[37];
    ph_uuid_v7(ph_now_wall_ns() / 1000000ull, ph_seed_u64(), 0, uuid);
    copy_str(out, PH_DISTINCT_ID_CAP, uuid);
}

void ph_log(ph_log_level level, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    if (!g_ph.on_log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_ph.on_log(level, buf, g_ph.user_data);
}

/* --- configuration ---------------------------------------------------- */

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

/* --- lifecycle -------------------------------------------------------- */

ph_result ph_init(const ph_config *cfg) {
    if (g_ph.initialized) return PH_ERR;
    if (!cfg) return PH_ERR_BADARG;

    memset(&g_ph, 0, sizeof(g_ph));

    copy_str(g_ph.api_key, PH_API_KEY_CAP, cfg->api_key);
    copy_str(g_ph.api_host, PH_HOST_CAP,
             (cfg->api_host && cfg->api_host[0]) ? cfg->api_host
                                                 : "https://us.i.posthog.com");
    copy_str(g_ph.release, PH_RELEASE_CAP, cfg->release);
    copy_str(g_ph.offline_path, PH_PATH_CAP, cfg->offline_path);
    g_ph.person_profiles = cfg->person_profiles;
    g_ph.flush_at = def_int(cfg->flush_at, 20);
    g_ph.flush_interval_ms = def_int(cfg->flush_interval_ms, 30000);
    g_ph.max_batch = def_int(cfg->max_batch, 50);
    g_ph.max_queue = def_int(cfg->max_queue, 1000);
    g_ph.request_timeout_ms = def_int(cfg->request_timeout_ms, 10000);
    g_ph.max_retries = cfg->max_retries >= 0 ? cfg->max_retries : 3;
    g_ph.gzip = cfg->gzip;
    g_ph.send_feature_flag_events = cfg->send_feature_flag_events;
    g_ph.before_send = cfg->before_send;
    g_ph.on_log = cfg->on_log;
    g_ph.user_data = cfg->user_data;

    ph_mutex_init(&g_ph.lock);
    ph_mutex_init(&g_ph.flush_lock);
    ph_cond_init(&g_ph.idle_cond);
    ph_props_init(&g_ph.super);
    atomic_init(&g_ph.seq, 0);

    if (cfg->distinct_id && cfg->distinct_id[0])
        copy_str(g_ph.distinct_id, PH_DISTINCT_ID_CAP, cfg->distinct_id);
    else
        gen_anon_id(g_ph.distinct_id); /* v0.1: memory-only; host owns persistence */
    g_ph.identified = 0;

    /* One clock reading; every event's wall time is reconstructed from it. */
    g_ph.epoch_wall_ns = ph_now_wall_ns();
    g_ph.epoch_mono_ns = ph_now_mono_ns();
    g_ph.uuid_salt = ph_seed_u64();

    /* Privacy denylist. */
    g_ph.denylist_count = 0;
    if (cfg->property_denylist && cfg->property_denylist_count > 0) {
        int i, n = cfg->property_denylist_count;
        if (n > PH_MAX_DENYLIST) n = PH_MAX_DENYLIST;
        for (i = 0; i < n; i++) {
            if (cfg->property_denylist[i] && cfg->property_denylist[i][0])
                copy_str(g_ph.denylist[g_ph.denylist_count++], PH_KEY_CAP,
                         cfg->property_denylist[i]);
        }
    }

    /* Token bucket: start full so an initial burst up to the rate passes. */
    g_ph.rl_rate = cfg->rate_limit_per_sec > 0 ? (double)cfg->rate_limit_per_sec : 0.0;
    g_ph.rl_burst = g_ph.rl_rate;
    g_ph.rl_tokens = g_ph.rl_burst;
    g_ph.rl_last_mono = g_ph.epoch_mono_ns;
    g_ph.rl_dropped = 0;

    g_ph.initialized = 1;

    if (!cfg->enabled) {
        /* No-op mode: initialized but no queue/thread; every call returns
         * quietly. Mutexes are still created so shutdown is uniform. */
        g_ph.enabled = 0;
        return PH_OK;
    }

    if (ph_queue_init(&g_ph.queue, g_ph.max_queue) != 0) {
        ph_cond_destroy(&g_ph.idle_cond);
        ph_mutex_destroy(&g_ph.flush_lock);
        ph_mutex_destroy(&g_ph.lock);
        g_ph.initialized = 0;
        return PH_ERR;
    }

    g_ph.transport = ph_http_transport_create();
    g_ph.enabled = 1;
    ph__sender_start();

    /* Warn at startup if https is configured but this build has no TLS backend
     * for the platform - otherwise every batch would silently fail. */
    if (strncmp(g_ph.api_host, "https://", 8) == 0 && !ph_tls_available())
        ph_log(PH_LOG_WARN, "api_host is https:// but no TLS backend is built for "
                            "this platform; batches will fail (use an http:// proxy)");

    /* signal_crash (v0.6): first replay any crash a previous run persisted - it
     * ships as a $exception through the normal path - then arm the handler for
     * this run. Needs offline_path to survive the restart. */
    if (cfg->crash_handler) {
        if (g_ph.offline_path[0]) {
            ph_signal_crash_replay(g_ph.offline_path);
            ph_signal_crash_install(g_ph.offline_path);
        } else {
            ph_log(PH_LOG_WARN, "crash_handler set but offline_path is empty; "
                                "signal_crash disabled");
        }
    }

    /* Preload flags so the first frame has answers. One blocking round
     * trip; opt out with preload_flags = 0. */
    if (cfg->preload_flags) ph__flags_fetch();
    return PH_OK;
}

void ph_shutdown(void) {
    if (!g_ph.initialized) return;
    ph_signal_crash_uninstall(); /* idempotent; restores prior fault disposition */
    if (g_ph.enabled) {
        ph__sender_stop_and_join();
        if (g_ph.transport.destroy) g_ph.transport.destroy(g_ph.transport.self);
        ph_queue_free(&g_ph.queue);
    }
    ph_cond_destroy(&g_ph.idle_cond);
    ph_mutex_destroy(&g_ph.flush_lock);
    ph_mutex_destroy(&g_ph.lock);
    memset(&g_ph, 0, sizeof(g_ph));
}

/* --- the shared enqueue path ------------------------------------------ */

/* profile_mode: -1 derive from policy, 0 force profile, 1 force anonymous.
 * `extra`/`extra_len`: optional pre-packed entries (e.g. the $exception_list
 * rawjson) appended after props/super/groups; pass NULL/0 when unused. */
static void submit_event(int kind, unsigned char base_flags, const char *name,
                         const char *did_override, const ph_props *props,
                         int profile_mode, int stamp_super_groups,
                         const char *extra, size_t extra_len) {
    uint64_t mono, seq;
    if (!g_ph.enabled || !name) return;

    /* Hot-path reads: one cheap monotonic tick + one atomic sequence bump.
     * No wall clock, no RNG, no malloc. */
    mono = ph_now_mono_ns();
    seq = (uint64_t)atomic_fetch_add(&g_ph.seq, (uint_least64_t)1);

    ph_mutex_lock(&g_ph.lock);
    {
        const char *did =
            (did_override && did_override[0]) ? did_override : g_ph.distinct_id;
        size_t cap = PH_EVENT_DATA_CAP;

        /* Rate limit product + exception events (not rare control events).
         * Refills from the monotonic tick we already read - no wall clock. */
        if (g_ph.rl_rate > 0.0 &&
            (kind == PH_EV_CAPTURE || kind == PH_EV_EXCEPTION)) {
            uint64_t d = mono >= g_ph.rl_last_mono ? mono - g_ph.rl_last_mono : 0;
            g_ph.rl_last_mono = mono;
            g_ph.rl_tokens += ((double)d / 1e9) * g_ph.rl_rate;
            if (g_ph.rl_tokens > g_ph.rl_burst) g_ph.rl_tokens = g_ph.rl_burst;
            if (g_ph.rl_tokens < 1.0) {
                g_ph.rl_dropped++;
                ph_mutex_unlock(&g_ph.lock);
                return;
            }
            g_ph.rl_tokens -= 1.0;
        }
        size_t name_len = strlen(name);
        size_t did_len = strlen(did);
        size_t off, bl;
        int no_profile;
        ph_event *e;

        if (profile_mode == 1)
            no_profile = 1;
        else if (profile_mode == 0)
            no_profile = 0;
        else
            no_profile = (g_ph.person_profiles == PH_NEVER) ||
                         (g_ph.person_profiles == PH_IDENTIFIED_ONLY &&
                          !g_ph.identified);

        if (name_len > cap) name_len = cap;
        if (did_len > cap - name_len) did_len = cap - name_len;

        e = ph_queue_begin_push(&g_ph.queue);
        e->kind = (uint8_t)kind;
        e->flags = (uint8_t)(base_flags | PH_EVF_HAS_DID |
                             (no_profile ? PH_EVF_NO_PROFILE : 0));
        e->mono_ns = mono;
        e->seq = seq;
        e->name_len = (uint16_t)name_len;
        e->did_len = (uint16_t)did_len;
        memcpy(e->data, name, name_len);
        memcpy(e->data + name_len, did, did_len);
        off = name_len + did_len;

        /* Raw structured extras (currently $exception_list) are prioritized so
         * diagnostic payloads survive even when user/super props crowd the fixed
         * blob. Super props come before explicit event props so callers win on
         * duplicate scalar keys (PostHog ingestion takes last-wins). */
        bl = 0;
        if (extra && extra_len && extra_len <= cap - off) {
            memcpy(e->data + off + bl, extra, extra_len);
            bl += extra_len;
        }
        if (stamp_super_groups)
            bl += ph_pack_props(&g_ph.super, e->data + off + bl, cap - off - bl);
        if (props)
            bl += ph_pack_props(props, e->data + off + bl, cap - off - bl);
        if (stamp_super_groups) {
            int gi;
            for (gi = 0; gi < g_ph.group_count; gi++)
                bl += ph_pack_str_entry(e->data + off + bl, cap - off - bl,
                                        (unsigned char)PH_PK_GROUP,
                                        g_ph.group_types[gi], g_ph.group_keys[gi]);
        }
        e->blob_len = (uint16_t)bl;
        ph_queue_end_push(&g_ph.queue);
    }
    ph_mutex_unlock(&g_ph.lock);

    if (ph_queue_size(&g_ph.queue) >= g_ph.flush_at) ph__sender_wake();
}

/* --- public capture surface ------------------------------------------- */

void ph_capture(const char *event, const ph_props *props) {
    if (!g_ph.enabled || !event || !event[0]) return;
    submit_event(PH_EV_CAPTURE, 0, event, NULL, props, -1, 1, NULL, 0);
}

void ph_identify(const char *distinct_id, const ph_props *set_props) {
    if (!g_ph.enabled || !distinct_id || !distinct_id[0]) return;
    ph_mutex_lock(&g_ph.lock);
    copy_str(g_ph.distinct_id, PH_DISTINCT_ID_CAP, distinct_id);
    g_ph.identified = 1;
    ph_mutex_unlock(&g_ph.lock);
    /* $identify builds a profile regardless of policy (profile_mode = 0). */
    submit_event(PH_EV_IDENTIFY, 0, "$identify", distinct_id, set_props, 0, 0, NULL, 0);

    /* Identity changed -> re-evaluate flags. Done on the sender thread so
     * ph_identify never blocks on the network. */
    ph_mutex_lock(&g_ph.flush_lock);
    g_ph.flags_refetch = 1;
    ph_mutex_unlock(&g_ph.flush_lock);
    ph__sender_wake();
}

void ph_alias(const char *new_id, const char *old_id) {
    ph_props p;
    if (!g_ph.enabled || !new_id || !new_id[0] || !old_id || !old_id[0]) return;
    ph_props_init(&p);
    ph_props_set_str(&p, "alias", new_id);
    submit_event(PH_EV_ALIAS, 0, "$create_alias", old_id, &p, 0, 0, NULL, 0);
}

void ph_reset(void) {
    if (!g_ph.enabled) return;
    ph_mutex_lock(&g_ph.lock);
    gen_anon_id(g_ph.distinct_id);
    g_ph.identified = 0;
    ph_props_init(&g_ph.super);
    g_ph.group_count = 0;
    ph_mutex_unlock(&g_ph.lock);
}

void ph_group(const char *type, const char *key, const ph_props *set_props) {
    ph_props p;
    char did[PH_KEY_CAP * 2 + 4];
    int i, found;
    if (!g_ph.enabled || !type || !type[0] || !key || !key[0]) return;

    /* Remember the membership so later events carry $groups. */
    ph_mutex_lock(&g_ph.lock);
    found = 0;
    for (i = 0; i < g_ph.group_count; i++) {
        if (strcmp(g_ph.group_types[i], type) == 0) {
            copy_str(g_ph.group_keys[i], PH_KEY_CAP, key);
            found = 1;
            break;
        }
    }
    if (!found && g_ph.group_count < PH_MAX_GROUPS) {
        copy_str(g_ph.group_types[g_ph.group_count], PH_KEY_CAP, type);
        copy_str(g_ph.group_keys[g_ph.group_count], PH_KEY_CAP, key);
        g_ph.group_count++;
    }
    ph_mutex_unlock(&g_ph.lock);

    /* Emit $groupidentify. $group_type/$group_key ride top-level; the set
     * props are bundled under $group_set by the serializer. */
    ph_props_init(&p);
    ph_props_set_str(&p, "$group_type", type);
    ph_props_set_str(&p, "$group_key", key);
    if (set_props) {
        for (i = 0; i < set_props->count; i++) {
            copy_prop_value(&p, &set_props->items[i]);
        }
    }
    snprintf(did, sizeof(did), "$%s_%s", type, key);
    submit_event(PH_EV_GROUP, 0, "$groupidentify", did, &p, 0, 0, NULL, 0);
}

void ph_register(const ph_props *super_props) {
    int i, j;
    if (!g_ph.enabled || !super_props) return;
    ph_mutex_lock(&g_ph.lock);
    for (i = 0; i < super_props->count; i++) {
        const ph_prop *src = &super_props->items[i];
        int found = 0;
        for (j = 0; j < g_ph.super.count; j++) {
            if (strcmp(g_ph.super.items[j].key, src->key) == 0) {
                g_ph.super.items[j] = *src;
                found = 1;
                break;
            }
        }
        if (!found && g_ph.super.count < PH_MAX_PROPS)
            g_ph.super.items[g_ph.super.count++] = *src;
    }
    ph_mutex_unlock(&g_ph.lock);
}

void ph_unregister(const char *key) {
    int j, k;
    if (!g_ph.enabled || !key) return;
    ph_mutex_lock(&g_ph.lock);
    for (j = 0; j < g_ph.super.count; j++) {
        if (strcmp(g_ph.super.items[j].key, key) == 0) {
            for (k = j; k + 1 < g_ph.super.count; k++)
                g_ph.super.items[k] = g_ph.super.items[k + 1];
            g_ph.super.count--;
            break;
        }
    }
    ph_mutex_unlock(&g_ph.lock);
}

/* Emit s as a JSON string literal, truncated to PH_EXCEPTION_FIELD_CAP bytes. */
static void json_cstr_exception_cap(ph_strbuf *out, const char *s) {
    size_t n = 0;
    if (!s) s = "";
    while (s[n] && n + 1 < PH_EXCEPTION_FIELD_CAP) n++;
    ph_json_str(out, s, n);
}

/* Build the $exception_list JSON array: one exception object with type,
 * value, mechanism, and a bounded raw stacktrace. Built here (off the sim hot
 * path - exceptions are rare) so the caller's transient frame pointers are
 * copied out before they go invalid. */
static void build_exception_list(ph_strbuf *out, const ph_exception *ex,
                                 const char *type, const char *message,
                                 int omit_function, int omit_filename,
                                 int omit_module, int omit_frames) {
    int i;
    int frame_count = ex->frame_count;
    if (frame_count < 0) frame_count = 0;
    if (frame_count > PH_MAX_EXCEPTION_FRAMES) frame_count = PH_MAX_EXCEPTION_FRAMES;

    ph_strbuf_append_cstr(out, "[{\"type\":");
    json_cstr_exception_cap(out, type ? type : "Error");
    ph_strbuf_append_cstr(out, ",\"value\":");
    json_cstr_exception_cap(out, message ? message : "");
    ph_strbuf_append_cstr(out, ",\"mechanism\":{\"handled\":");
    ph_json_bool(out, ex->handled);
    ph_strbuf_append_cstr(out, ",\"synthetic\":");
    ph_json_bool(out, ex->synthetic);
    ph_strbuf_append_cstr(out, "},\"stacktrace\":{\"type\":\"raw\",\"frames\":[");
    for (i = 0; !omit_frames && ex->frames && i < frame_count; i++) {
        const ph_stackframe *f = &ex->frames[i];
        /* Stop before a frame would push the payload past the event blob, so a
         * deep stack degrades to as-many-as-fit rather than the packer dropping
         * the whole $exception_list. Checked before the separator so we never
         * leave a trailing comma. */
        if (out->len > (size_t)PH_EVENT_DATA_CAP - PH_EXCEPTION_BLOB_RESERVE) break;
        if (i > 0) ph_strbuf_append_char(out, ',');
        ph_strbuf_append_cstr(out, "{\"platform\":\"custom\",\"lang\":\"cpp\",\"in_app\":");
        ph_json_bool(out, f->in_app);
        if (!omit_function && f->function) {
            ph_strbuf_append_cstr(out, ",\"function\":");
            json_cstr_exception_cap(out, f->function);
        }
        if (!omit_filename && f->filename) {
            ph_strbuf_append_cstr(out, ",\"filename\":");
            json_cstr_exception_cap(out, f->filename);
        }
        if (!omit_module && f->module) {
            ph_strbuf_append_cstr(out, ",\"module\":");
            json_cstr_exception_cap(out, f->module);
        }
        if (f->lineno > 0) { ph_strbuf_append_cstr(out, ",\"lineno\":"); ph_json_int(out, f->lineno); }
        ph_strbuf_append_cstr(out, ",\"resolved\":true}");
    }
    ph_strbuf_append_cstr(out, "]}}]");
}

/* Build the non-frame $exception props, then extract the (possibly scrubbed)
 * type/message for the caller. Applies the denylist and runs before_send:
 * returns 0 if before_send vetoed the event (caller drops it), else 1. The
 * omit_* out-flags report which frame fields the denylist suppresses. */
static int prepare_exception_props(const ph_exception *ex, ph_props *p,
                                   char *type, size_t type_cap,
                                   char *message, size_t message_cap,
                                   int *omit_function, int *omit_filename,
                                   int *omit_module, int *omit_frames) {
    const char *v;
    int i;

    ph_props_init(p);
    ph_props_set_str(p, "$exception_level", ex->handled ? "warning" : "error");
    ph_props_set_str(p, "$exception_type", ex->type ? ex->type : "Error");
    ph_props_set_str(p, "$exception_message", ex->message ? ex->message : "");
    if (ex->extra) {
        for (i = 0; i < ex->extra->count; i++) copy_prop_value(p, &ex->extra->items[i]);
    }

    apply_denylist(p);
    if (denylist_has("type")) remove_prop_key(p, "$exception_type");
    if (denylist_has("message")) remove_prop_key(p, "$exception_message");

    if (g_ph.before_send && !g_ph.before_send("$exception", p, g_ph.user_data))
        return 0;

    v = find_last_str_prop(p, "$exception_type");
    copy_str(type, type_cap, v ? v : "Error");
    v = find_last_str_prop(p, "$exception_message");
    copy_str(message, message_cap, v ? v : "");

    remove_prop_key(p, "$exception_type");
    remove_prop_key(p, "$exception_message");

    *omit_function = denylist_has("function") || denylist_has("$exception_frame_function");
    *omit_filename = denylist_has("filename") || denylist_has("$exception_frame_filename");
    *omit_module = denylist_has("module") || denylist_has("$exception_frame_module");
    *omit_frames = denylist_has("frames") || denylist_has("stacktrace") ||
                   denylist_has("$exception_frames");
    return 1;
}

void ph_capture_exception(const ph_exception *ex) {
    ph_props p;
    ph_strbuf list;
    char extra[PH_EVENT_DATA_CAP];
    size_t extra_len = 0;
    char type[PH_EXCEPTION_FIELD_CAP];
    char message[PH_EXCEPTION_FIELD_CAP];
    int omit_function, omit_filename, omit_module, omit_frames;

    if (!g_ph.enabled || !ex) return;

    /* The structured exception payload has to be copied before caller-owned
     * pointers go stale. Run the privacy hook here for exceptions so type/message
     * can be redacted before the raw $exception_list is built. */
    if (!prepare_exception_props(ex, &p, type, sizeof(type), message, sizeof(message),
                                 &omit_function, &omit_filename, &omit_module,
                                 &omit_frames))
        return;

    /* The nested $exception_list rides as a rawjson entry (the flat packer can't
     * express nested arrays/objects); the serializer emits it verbatim. */
    ph_strbuf_init(&list);
    build_exception_list(&list, ex, type, message, omit_function, omit_filename,
                         omit_module, omit_frames);
    if (list.data && !list.oom)
        extra_len = ph_pack_str_entry(extra, sizeof(extra), (unsigned char)PH_PK_RAWJSON,
                                      "$exception_list", list.data);

    submit_event(PH_EV_EXCEPTION, PH_EVF_SCRUBBED, "$exception", NULL, &p, -1, 1,
                 extra, extra_len);
    ph_strbuf_free(&list);
}

uint64_t ph_dropped_events(void) {
    uint64_t rate_dropped;
    uint64_t ring_dropped = 0;
    if (!g_ph.initialized) return 0;
    if (g_ph.enabled) ring_dropped = ph_queue_dropped(&g_ph.queue);
    ph_mutex_lock(&g_ph.lock);
    rate_dropped = g_ph.rl_dropped;
    ph_mutex_unlock(&g_ph.lock);
    return ring_dropped + rate_dropped;
}

/* --- feature flags ---------------------------------------------------- */

/* Deduped exposure event emitted when a flag is read (ph_flags.c calls this). */
void ph__emit_ff_called(const char *key, const char *value) {
    ph_props p;
    if (!g_ph.enabled) return;
    ph_props_init(&p);
    ph_props_set_str(&p, "$feature_flag", key);
    ph_props_set_str(&p, "$feature_flag_response", value);
    submit_event(PH_EV_CAPTURE, 0, "$feature_flag_called", NULL, &p, -1, 1, NULL, 0);
}

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

void ph_reload_feature_flags(void) {
    if (!g_ph.enabled) return;
    ph__flags_fetch();
}
