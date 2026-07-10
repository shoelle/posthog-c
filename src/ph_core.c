/*
 * ph_core.c - the SDK instance, lifecycle, and the shared enqueue path.
 *
 * Holds the process-global ph_ctx and the core public API: init/shutdown,
 * capture, and the identity / super-property / group calls. Every event emitter
 * funnels through ph__submit_event(), which snapshots identity / super-properties
 * / group scoping under one mutex and packs a self-contained event into the ring.
 * Because each event bakes its own distinct_id and profile decision at capture
 * time, the sender needs no shared identity state, and the ordering "events
 * captured before ph_identify keep the anonymous id" falls out for free.
 *
 * The $exception path lives in ph_exception.c and feature flags in ph_flags.c;
 * both call ph__submit_event() here.
 */
#include "ph_internal.h"
#include "ph_crash.h"
#include "ph_http.h"
#include "ph_time.h"
#include "ph_tls.h"
#include "ph_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ph_ctx g_ph;
_Thread_local int ph__in_callback;

const char *ph_version(void) { return PH_VERSION_STRING; }

/* --- small helpers ---------------------------------------------------- */

static int def_int(int v, int fallback) { return v > 0 ? v : fallback; }

#define PH_CONFIG_MAX_BATCH 10000
#define PH_CONFIG_MAX_QUEUE 100000

static int string_over_cap(const char *s, size_t cap) {
    return s && strlen(s) >= cap;
}

static ph_result validate_config(const ph_config *cfg) {
    int i;
    const char *host;
    if (!cfg) return PH_ERR_BADARG;
    if (!cfg->enabled) return PH_OK;
    if (!cfg->api_key || !cfg->api_key[0]) return PH_ERR_BADARG;
    if (!cfg->distinct_id || !cfg->distinct_id[0]) return PH_ERR_BADARG;
    host = (cfg->api_host && cfg->api_host[0]) ? cfg->api_host
                                               : "https://us.i.posthog.com";
    if (strncmp(host, "http://", 7) != 0 && strncmp(host, "https://", 8) != 0)
        return PH_ERR_BADARG;
    if (string_over_cap(cfg->api_key, PH_API_KEY_CAP) ||
        string_over_cap(host, PH_HOST_CAP) ||
        string_over_cap(cfg->release, PH_RELEASE_CAP) ||
        string_over_cap(cfg->offline_path, PH_PATH_CAP) ||
        string_over_cap(cfg->distinct_id, PH_DISTINCT_ID_CAP))
        return PH_ERR_BADARG;
    if (cfg->person_profiles < PH_IDENTIFIED_ONLY || cfg->person_profiles > PH_NEVER)
        return PH_ERR_BADARG;
    if (cfg->flush_at < 0 || cfg->flush_interval_ms < 0 || cfg->max_batch < 0 ||
        cfg->max_queue < 0 || cfg->request_timeout_ms < 0 || cfg->max_retries < 0 ||
        cfg->max_batch_bytes < 0 || cfg->stats_interval_ms < 0)
        return PH_ERR_BADARG;
    if (def_int(cfg->max_batch, 50) > PH_CONFIG_MAX_BATCH ||
        def_int(cfg->max_queue, 1000) > PH_CONFIG_MAX_QUEUE)
        return PH_ERR_BADARG;
    if (cfg->property_denylist_count < 0 ||
        cfg->property_denylist_count > PH_MAX_DENYLIST)
        return PH_ERR_BADARG;
    if (cfg->property_denylist_count > 0 && !cfg->property_denylist)
        return PH_ERR_BADARG;
    for (i = 0; cfg->property_denylist && i < cfg->property_denylist_count; i++)
        if (string_over_cap(cfg->property_denylist[i], PH_KEY_CAP))
            return PH_ERR_BADARG;
    return PH_OK;
}

static void gen_anon_id(char *out) {
    char uuid[37];
    ph_uuid_v7(ph_now_wall_ns() / 1000000ull, ph_seed_u64(), 0, uuid);
    ph_copy_capped(out, PH_DISTINCT_ID_CAP, uuid);
}

/* Caller holds g_ph.lock. A flag value is meaningful only for the exact
 * identity + group context that produced it. Clear synchronously so a caller
 * can never observe the previous user's value while an async refresh runs. */
static void flags_context_changed_locked(void) {
    g_ph.flag_count = 0;
    g_ph.flags_context_gen++;
    if (g_ph.flags_context_gen == 0) g_ph.flags_context_gen = 1;
}

static void request_flags_refetch(void) {
    ph_mutex_lock(&g_ph.flush_lock);
    g_ph.flags_refetch = 1;
    ph_mutex_unlock(&g_ph.flush_lock);
    ph__sender_wake();
}

void ph_log(ph_log_level level, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    if (!g_ph.on_log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ph__in_callback++;
    g_ph.on_log(level, buf, g_ph.user_data);
    ph__in_callback--;
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
    cfg->max_batch_bytes = 1024 * 1024; /* 1 MiB; 0 disables the per-POST byte cap */
    /* on_stats NULL + stats_interval_ms 0 => health snapshots off by default */
}

/* --- lifecycle -------------------------------------------------------- */

ph_result ph_init(const ph_config *cfg) {
    ph_result valid;
    if (g_ph.initialized) return PH_ERR;
    valid = validate_config(cfg);
    if (valid != PH_OK) return valid;

    memset(&g_ph, 0, sizeof(g_ph));

    ph_copy_capped(g_ph.api_key, PH_API_KEY_CAP, cfg->api_key);
    ph_copy_capped(g_ph.api_host, PH_HOST_CAP,
             (cfg->api_host && cfg->api_host[0]) ? cfg->api_host
                                                 : "https://us.i.posthog.com");
    ph_copy_capped(g_ph.release, PH_RELEASE_CAP, cfg->release);
    ph_copy_capped(g_ph.offline_path, PH_PATH_CAP, cfg->offline_path);
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
    g_ph.on_stats = cfg->on_stats;
    g_ph.stats_interval_ms = cfg->stats_interval_ms;
    g_ph.max_batch_bytes = cfg->max_batch_bytes > 0 ? cfg->max_batch_bytes : 0;

    ph_mutex_init(&g_ph.lock);
    ph_mutex_init(&g_ph.flush_lock);
    ph_cond_init(&g_ph.idle_cond);
    ph_props_init(&g_ph.super);
    ph_props_init(&g_ph.flag_person_props);
    atomic_init(&g_ph.seq, 0);
    atomic_init(&g_ph.st_sent, 0);
    atomic_init(&g_ph.st_failed, 0);
    atomic_init(&g_ph.st_retries, 0);
    atomic_init(&g_ph.st_before_send_dropped, 0);
    g_ph.flags_context_gen = 1;

    ph_copy_capped(g_ph.distinct_id, PH_DISTINCT_ID_CAP, cfg->distinct_id);
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
                ph_copy_capped(g_ph.denylist[g_ph.denylist_count++], PH_KEY_CAP,
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
    if (ph__sender_start() != 0) {
        if (g_ph.transport.destroy) g_ph.transport.destroy(g_ph.transport.self);
        ph_queue_free(&g_ph.queue);
        ph_cond_destroy(&g_ph.idle_cond);
        ph_mutex_destroy(&g_ph.flush_lock);
        ph_mutex_destroy(&g_ph.lock);
        memset(&g_ph, 0, sizeof(g_ph));
        return PH_ERR;
    }

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
    /* on_log/on_stats execute on the sender thread. A callback cannot tear down
     * the storage and synchronization primitives beneath its own stack. */
    if (ph__in_callback ||
        (g_ph.sender_running && ph_thread_is_current(&g_ph.sender)))
        return;
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

/* Token-bucket admission for product/exception events (control events are never
 * limited). Refills from the monotonic tick the caller already read, so the hot
 * path stays wall-clock/RNG-free. Caller holds g_ph.lock. Returns 0 if the event
 * should be dropped (bucket empty), else 1 (a token was consumed). */
static int rate_limit_admit(int kind, uint64_t mono) {
    uint64_t d;
    if (g_ph.rl_rate <= 0.0 ||
        (kind != PH_EV_CAPTURE && kind != PH_EV_EXCEPTION))
        return 1; /* unlimited, or a rare control event */
    d = mono >= g_ph.rl_last_mono ? mono - g_ph.rl_last_mono : 0;
    g_ph.rl_last_mono = mono;
    g_ph.rl_tokens += ((double)d / 1e9) * g_ph.rl_rate;
    if (g_ph.rl_tokens > g_ph.rl_burst) g_ph.rl_tokens = g_ph.rl_burst;
    if (g_ph.rl_tokens < 1.0) {
        g_ph.rl_dropped++;
        return 0;
    }
    g_ph.rl_tokens -= 1.0;
    return 1;
}

/* Resolve whether this event suppresses its person profile: profile_mode 1
 * forces anonymous, 0 forces a profile, -1 derives from the configured policy
 * and current identity. Caller holds g_ph.lock. */
static int derive_no_profile(int profile_mode) {
    if (profile_mode == 1) return 1;
    if (profile_mode == 0) return 0;
    return (g_ph.person_profiles == PH_NEVER) ||
           (g_ph.person_profiles == PH_IDENTIFIED_ONLY && !g_ph.identified);
}

/* profile_mode: -1 derive from policy, 0 force profile, 1 force anonymous.
 * `extra`/`extra_len`: optional pre-packed entries (e.g. the $exception_list
 * rawjson) appended after props/super/groups; pass NULL/0 when unused. */
ph_result ph__submit_event(int kind, unsigned char base_flags, const char *name,
                         const char *did_override, const ph_props *props,
                         int profile_mode, int stamp_super_groups,
                         const char *extra, size_t extra_len) {
    uint64_t mono, seq;
    if (!g_ph.enabled) return PH_ERR_DISABLED;
    if (!name) return PH_ERR_BADARG;

    /* Hot-path reads: one cheap monotonic tick + one atomic sequence bump.
     * No wall clock, no RNG, no malloc. */
    mono = ph_now_mono_ns();
    seq = (uint64_t)atomic_fetch_add(&g_ph.seq, (uint_least64_t)1);

    ph_mutex_lock(&g_ph.lock);
    {
        const char *did =
            (did_override && did_override[0]) ? did_override : g_ph.distinct_id;
        size_t cap = PH_EVENT_DATA_CAP;

        /* Token-bucket admission (product/exception events only). */
        if (!rate_limit_admit(kind, mono)) {
            ph_mutex_unlock(&g_ph.lock);
            return PH_ERR_RATE_LIMITED;
        }
        size_t name_len = strlen(name);
        size_t did_len = strlen(did);
        size_t off, bl;
        int no_profile;
        ph_event *e;
        ph_props merged;

        no_profile = derive_no_profile(profile_mode);

        if (name_len >= PH_EVENT_NAME_CAP) name_len = PH_EVENT_NAME_CAP - 1;
        if (did_len >= PH_DISTINCT_ID_CAP) did_len = PH_DISTINCT_ID_CAP - 1;
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
         * blob. Explicit event props are merged ahead of non-shadowed super
         * props so callers win while the fixed public cap stays deterministic. */
        bl = 0;
        if (extra && extra_len && extra_len <= cap - off) {
            memcpy(e->data + off + bl, extra, extra_len);
            bl += extra_len;
        }
        if (stamp_super_groups) {
            ph_props_merge(&merged, props, &g_ph.super);
            bl += ph_pack_props(&merged, e->data + off + bl, cap - off - bl);
        } else if (props) {
            bl += ph_pack_props(props, e->data + off + bl, cap - off - bl);
        }
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
    return PH_OK;
}

/* --- public capture surface ------------------------------------------- */

ph_result ph_capture(const char *event, const ph_props *props) {
    ph_result r;
    int truncated;
    if (!g_ph.enabled) return PH_ERR_DISABLED;
    if (!event || !event[0]) return PH_ERR_BADARG;
    truncated = strlen(event) >= PH_EVENT_NAME_CAP;
    r = ph__submit_event(PH_EV_CAPTURE, 0, event, NULL, props, -1, 1, NULL, 0);
    return r == PH_OK && truncated ? PH_ERR_TRUNCATED : r;
}

void ph_identify(const char *distinct_id, const ph_props *set_props) {
    if (!g_ph.enabled || !distinct_id || !distinct_id[0]) return;
    ph_mutex_lock(&g_ph.lock);
    ph_copy_capped(g_ph.distinct_id, PH_DISTINCT_ID_CAP, distinct_id);
    g_ph.identified = 1;
    if (set_props)
        g_ph.flag_person_props = *set_props;
    else
        ph_props_init(&g_ph.flag_person_props);
    flags_context_changed_locked();
    ph_mutex_unlock(&g_ph.lock);
    /* $identify builds a profile regardless of policy (profile_mode = 0). */
    ph__submit_event(PH_EV_IDENTIFY, 0, "$identify", distinct_id, set_props, 0, 0, NULL, 0);

    /* Identity changed -> re-evaluate flags. Done on the sender thread so
     * ph_identify never blocks on the network. */
    request_flags_refetch();
}

void ph_alias(const char *new_id, const char *old_id) {
    ph_props p;
    char new_capped[PH_DISTINCT_ID_CAP];
    char old_capped[PH_DISTINCT_ID_CAP];
    if (!g_ph.enabled || !new_id || !new_id[0] || !old_id || !old_id[0]) return;
    ph_copy_capped(new_capped, sizeof(new_capped), new_id);
    ph_copy_capped(old_capped, sizeof(old_capped), old_id);
    ph_props_init(&p);
    ph_props_set_str(&p, "alias", new_capped);
    ph__submit_event(PH_EV_ALIAS, 0, "$create_alias", old_capped, &p, 0, 0, NULL, 0);
}

ph_result ph_get_distinct_id(char *out, int cap) {
    size_t len;
    if (!out || cap <= 0) return PH_ERR_BADARG;
    out[0] = '\0';
    if (!g_ph.enabled) return PH_ERR_DISABLED;
    ph_mutex_lock(&g_ph.lock);
    len = strlen(g_ph.distinct_id);
    ph_copy_capped(out, (size_t)cap, g_ph.distinct_id);
    ph_mutex_unlock(&g_ph.lock);
    return len >= (size_t)cap ? PH_ERR_TRUNCATED : PH_OK;
}

void ph_reset(void) {
    if (!g_ph.enabled) return;
    ph_mutex_lock(&g_ph.lock);
    gen_anon_id(g_ph.distinct_id);
    g_ph.identified = 0;
    ph_props_init(&g_ph.super);
    ph_props_init(&g_ph.flag_person_props);
    g_ph.group_count = 0;
    flags_context_changed_locked();
    ph_mutex_unlock(&g_ph.lock);
    request_flags_refetch();
}

void ph_group(const char *type, const char *key, const ph_props *set_props) {
    ph_props p;
    char did[PH_KEY_CAP * 2 + 4];
    char type_capped[PH_KEY_CAP];
    char key_capped[PH_KEY_CAP];
    int i, found, context_changed = 0;
    if (!g_ph.enabled || !type || !type[0] || !key || !key[0]) return;
    ph_copy_capped(type_capped, sizeof(type_capped), type);
    ph_copy_capped(key_capped, sizeof(key_capped), key);

    /* Remember the membership so later events carry $groups. */
    ph_mutex_lock(&g_ph.lock);
    found = 0;
    for (i = 0; i < g_ph.group_count; i++) {
        if (strcmp(g_ph.group_types[i], type_capped) == 0) {
            /* The key or the supplied group properties may affect evaluation;
             * invalidate even when the membership key itself is unchanged. */
            context_changed = 1;
            ph_copy_capped(g_ph.group_keys[i], PH_KEY_CAP, key_capped);
            if (set_props)
                g_ph.group_props[i] = *set_props;
            else
                ph_props_init(&g_ph.group_props[i]);
            found = 1;
            break;
        }
    }
    if (!found && g_ph.group_count < PH_MAX_GROUPS) {
        ph_copy_capped(g_ph.group_types[g_ph.group_count], PH_KEY_CAP, type_capped);
        ph_copy_capped(g_ph.group_keys[g_ph.group_count], PH_KEY_CAP, key_capped);
        if (set_props)
            g_ph.group_props[g_ph.group_count] = *set_props;
        else
            ph_props_init(&g_ph.group_props[g_ph.group_count]);
        g_ph.group_count++;
        context_changed = 1;
    }
    if (context_changed) flags_context_changed_locked();
    ph_mutex_unlock(&g_ph.lock);
    if (context_changed) request_flags_refetch();

    /* Emit $groupidentify. $group_type/$group_key ride top-level; the set
     * props are bundled under $group_set by the serializer. */
    ph_props_init(&p);
    ph_props_set_str(&p, "$group_type", type_capped);
    ph_props_set_str(&p, "$group_key", key_capped);
    if (set_props) {
        for (i = 0; i < set_props->count; i++) {
            ph_copy_prop_value(&p, &set_props->items[i]);
        }
    }
    snprintf(did, sizeof(did), "$%s_%s", type_capped, key_capped);
    ph__submit_event(PH_EV_GROUP, 0, "$groupidentify", did, &p, 0, 0, NULL, 0);
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

uint64_t ph_dropped_events(void) {
    uint64_t rate_dropped, scrub_dropped, delivery_failed;
    uint64_t ring_dropped = 0;
    if (!g_ph.initialized) return 0;
    if (g_ph.enabled) ring_dropped = ph_queue_dropped(&g_ph.queue);
    ph_mutex_lock(&g_ph.lock);
    rate_dropped = g_ph.rl_dropped;
    ph_mutex_unlock(&g_ph.lock);
    scrub_dropped = atomic_load(&g_ph.st_before_send_dropped);
    delivery_failed = atomic_load(&g_ph.st_failed);
    return ring_dropped + rate_dropped + scrub_dropped + delivery_failed;
}
