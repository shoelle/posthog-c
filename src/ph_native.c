/*
 * ph_native.c — the background sender thread and flush coordination.
 *
 * capture() only ever touches the ring (ph_core.c). Everything expensive for
 * product events — serialization, before_send, the network POST — happens here,
 * off the caller thread. Exception events pre-scrub their structured payload
 * before enqueue so raw exception text can be redacted. The loop parks on the
 * queue's condition variable until it has
 * flush_at events or flush_interval_ms elapses, then drains everything present
 * in max_batch-sized POSTs.
 */
#include "ph_internal.h"
#include "ph_str.h"
#include "ph_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* api_host + "/batch/", tolerating a trailing slash on the host. */
static void build_batch_url(char *out, size_t cap) {
    size_t n = strlen(g_ph.api_host);
    while (n > 0 && g_ph.api_host[n - 1] == '/') n--;
    snprintf(out, cap, "%.*s/batch/", (int)n, g_ph.api_host);
}

/* POST one already-serialized batch body. Returns the HTTP-ish status. */
static int post_body(const char *body, size_t len) {
    char url[PH_HOST_CAP + 16];
    ph_transport t;
    build_batch_url(url, sizeof(url));
    ph_mutex_lock(&g_ph.flush_lock);
    t = g_ph.transport;
    ph_mutex_unlock(&g_ph.flush_lock);
    if (!t.send) return -1;
    return t.send(t.self, url, body, len, g_ph.request_timeout_ms);
}

/* --- Offline spill file (§6) ------------------------------------------
 *
 * When a send fails and offline_path is set, the serialized batch is appended
 * as one line to a single NDJSON file. On the next drain the sender replays
 * the file (oldest first) before new memory events, so an app that runs
 * offline for a long stretch loses nothing when it reconnects.
 */

static void ensure_offline_dir(void) {
#if defined(_WIN32)
    _mkdir(g_ph.offline_path);
#else
    mkdir(g_ph.offline_path, 0777);
#endif
}

static void offline_spill_path(char *out, size_t cap) {
    size_t n = strlen(g_ph.offline_path);
    while (n > 0 && (g_ph.offline_path[n - 1] == '/' || g_ph.offline_path[n - 1] == '\\'))
        n--;
    snprintf(out, cap, "%.*s/%s", (int)n, g_ph.offline_path, PH_OFFLINE_FILENAME);
}

static long file_size(const char *path) {
    long sz;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fclose(f);
    return sz;
}

/* Keep the spill file under the byte cap by dropping the oldest whole lines. */
static void offline_enforce_cap(const char *path, size_t incoming) {
    long sz = file_size(path);
    char *buf;
    size_t rd, target, drop;
    FILE *f;

    if (sz < 0) return;
    if ((size_t)sz + incoming + 1 <= (size_t)PH_OFFLINE_MAX_BYTES) return;

    f = fopen(path, "rb");
    if (!f) return;
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    /* Keep at most (cap - incoming) newest bytes, aligned to a line boundary. */
    target = (size_t)PH_OFFLINE_MAX_BYTES > incoming + 1
                 ? (size_t)PH_OFFLINE_MAX_BYTES - incoming - 1
                 : 0;
    drop = rd > target ? rd - target : 0;
    while (drop < rd && buf[drop] != '\n') drop++;
    if (drop < rd) drop++; /* step past the newline */

    f = fopen(path, "wb");
    if (f) {
        if (rd > drop) fwrite(buf + drop, 1, rd - drop, f);
        fclose(f);
    }
    free(buf);
}

static void offline_spill(const char *body, size_t len) {
    char path[PH_PATH_CAP + 32];
    FILE *f;
    if (!g_ph.offline_path[0] || len == 0) return;
    ensure_offline_dir();
    offline_spill_path(path, sizeof(path));
    offline_enforce_cap(path, len);
    f = fopen(path, "ab");
    if (!f) {
        ph_log(PH_LOG_WARN, "offline: cannot open spill file %s", path);
        return;
    }
    fwrite(body, 1, len, f);
    fputc('\n', f);
    fclose(f);
}

/* Replay spilled batches oldest-first. On the first failure, stop and keep the
 * rest (still offline) so order is preserved and we don't hammer the network. */
static void offline_replay(void) {
    char path[PH_PATH_CAP + 32];
    long sz;
    FILE *f;
    char *buf;
    size_t rd, i, line_start;
    int failed = 0;
    ph_strbuf keep;

    if (!g_ph.offline_path[0]) return;
    offline_spill_path(path, sizeof(path));
    sz = file_size(path);
    if (sz <= 0) return;

    f = fopen(path, "rb");
    if (!f) return;
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    ph_strbuf_init(&keep);
    line_start = 0;
    for (i = 0; i <= rd; i++) {
        if (i != rd && buf[i] != '\n') continue;
        if (i > line_start) {
            size_t linelen = i - line_start;
            int keep_line = failed;
            if (!failed) {
                int status = post_body(buf + line_start, linelen);
                if (status < 200 || status >= 300) {
                    failed = 1;
                    keep_line = 1;
                }
            }
            if (keep_line) {
                ph_strbuf_append(&keep, buf + line_start, linelen);
                ph_strbuf_append_char(&keep, '\n');
            }
        }
        line_start = i + 1;
    }
    free(buf);

    if (keep.len == 0) {
        remove(path);
    } else {
        f = fopen(path, "wb");
        if (f) {
            fwrite(keep.data, 1, keep.len, f);
            fclose(f);
        }
    }
    ph_strbuf_free(&keep);
}

/* Remove every entry with `key` from a ph_props (in place). */
static void remove_key(ph_props *p, const char *key) {
    int i, k;
    for (i = 0; i < p->count;) {
        if (strcmp(p->items[i].key, key) == 0) {
            for (k = i; k + 1 < p->count; k++) p->items[k] = p->items[k + 1];
            p->count--;
        } else {
            i++;
        }
    }
}

/* Scrub one event: strip denylisted keys, and (when run_before_send is set) run
 * before_send. Returns 1 to keep (blob rewritten in place) or 0 to drop.
 * Non-scalar entries ($groups, $exception_list) are preserved untouched. */
static int scrub_one(ph_event *e, int run_before_send) {
    char name[128];
    char *blob = e->data + e->name_len + e->did_len;
    size_t len = e->blob_len;
    ph_props props;
    char preserve[PH_EVENT_DATA_CAP]; /* verbatim bytes of non-scalar entries */
    size_t plen = 0;
    char tmp[PH_EVENT_DATA_CAP];
    size_t nb, avail, nl;
    int i;
    const char *cur = blob, *cend = blob + len;
    unsigned char type;
    const char *key, *val;
    size_t klen, vlen;

    nl = e->name_len < sizeof(name) - 1 ? e->name_len : sizeof(name) - 1;
    memcpy(name, e->data, nl);
    name[nl] = '\0';

    /* Scalars -> props (scrubbable). Non-scalar entries ($groups, the
     * $exception_list rawjson) are copied through verbatim: before_send only
     * shapes user properties, and those payloads must survive the round trip. */
    ph_unpack_props(blob, len, &props);
    while (ph_blob_next(&cur, cend, &type, &key, &klen, &val, &vlen)) {
        if (type <= PH_T_BOOL) continue; /* scalar: already captured in props */
        {
            const char *entry = key - 4; /* the 4-byte header precedes the key */
            size_t entry_len = 4 + klen + vlen;
            if (plen + entry_len <= sizeof(preserve)) {
                memcpy(preserve + plen, entry, entry_len);
                plen += entry_len;
            }
        }
    }

    for (i = 0; i < g_ph.denylist_count; i++) remove_key(&props, g_ph.denylist[i]);
    if (run_before_send && g_ph.before_send &&
        !g_ph.before_send(name, &props, g_ph.user_data))
        return 0;

    nb = ph_pack_props(&props, tmp, sizeof(tmp));
    if (plen && nb + plen <= sizeof(tmp)) {
        memcpy(tmp + nb, preserve, plen);
        nb += plen;
    }
    avail = (size_t)PH_EVENT_DATA_CAP - e->name_len - e->did_len;
    if (nb > avail) nb = avail;
    memcpy(blob, tmp, nb);
    e->blob_len = (uint16_t)nb;
    return 1;
}

/* Apply the scrub stage across a batch, compacting out dropped events. Returns
 * the surviving count. A true no-op when no denylist / before_send is set. */
static int scrub_events(ph_event *evs, int n) {
    int i, out = 0;
    if (!g_ph.before_send && g_ph.denylist_count == 0) return n;
    for (i = 0; i < n; i++) {
        int kind = evs[i].kind;
        if (kind == PH_EV_CAPTURE || kind == PH_EV_EXCEPTION) {
            /* Exceptions pre-scrub at capture (before_send already ran), but
             * super props / groups are stamped afterward — so still re-apply the
             * denylist to those, just don't run the hook a second time. */
            if (evs[i].flags & PH_EVF_SCRUBBED) {
                if (g_ph.denylist_count > 0) scrub_one(&evs[i], 0);
            } else if (!scrub_one(&evs[i], 1)) {
                continue; /* dropped by before_send */
            }
        }
        if (out != i) evs[out] = evs[i];
        out++;
    }
    return out;
}

/* Sleep up to `ms` in small chunks, returning 1 the moment shutdown is
 * requested — so retry backoff never delays ph_shutdown by the full schedule. */
static int sender_backoff_wait(int ms) {
    int slept = 0;
    while (slept < ms) {
        int stop, chunk;
        ph_mutex_lock(&g_ph.flush_lock);
        stop = g_ph.stop;
        ph_mutex_unlock(&g_ph.flush_lock);
        if (stop) return 1;
        chunk = ms - slept;
        if (chunk > 50) chunk = 50;
        ph_sleep_ms(chunk);
        slept += chunk;
    }
    return 0;
}

static void send_batch(ph_event *evs, int n) {
    ph_strbuf body;
    int status = -1;
    int attempt, attempts;

    n = scrub_events(evs, n);
    if (n == 0) return; /* every event was scrubbed away */

    ph_strbuf_init(&body);
    ph_serialize_batch(&g_ph, evs, n, &body);
    if (body.oom) {
        ph_strbuf_free(&body);
        ph_log(PH_LOG_ERROR, "serialize OOM; dropped %d events", n);
        return;
    }

    /* Retry only failures a retry can plausibly fix: 5xx, timeouts, and network
     * errors (status < 0). 2xx succeeded; 4xx is a deterministic client error
     * (bad key/body) that won't change. Exponential backoff between attempts,
     * interruptible on shutdown. */
    attempts = g_ph.max_retries >= 0 ? g_ph.max_retries + 1 : 1;
    for (attempt = 0; attempt < attempts; attempt++) {
        if (attempt > 0) {
            int shift = attempt - 1 > 6 ? 6 : attempt - 1; /* clamp: avoid overflow */
            int backoff = 100 << shift;
            if (backoff > 2000) backoff = 2000;
            if (sender_backoff_wait(backoff)) break; /* shutdown requested */
        }
        status = post_body(body.data ? body.data : "", body.len);
        if (status >= 200 && status < 300) break; /* delivered */
        if (status >= 400 && status < 500) break; /* client error: won't change */
    }
    if (status < 200 || status >= 300) {
        if (g_ph.offline_path[0]) {
            offline_spill(body.data ? body.data : "", body.len);
            ph_log(PH_LOG_INFO, "batch spilled to offline queue (status %d)", status);
        } else {
            ph_log(PH_LOG_WARN,
                   "batch POST failed (status %d); %d events dropped "
                   "(set offline_path to persist failed batches)",
                   status, n);
        }
    }

    ph_strbuf_free(&body);
}

/* Drain: replay any spilled batches first (preserving order across a
 * reconnect), then send everything currently in memory, in max_batch chunks.
 * `sending` covers the whole pass so ph_flush() waits for the replay too. */
static void drain(ph_event *scratch) {
    ph_mutex_lock(&g_ph.flush_lock);
    g_ph.sending = 1;
    ph_mutex_unlock(&g_ph.flush_lock);

    offline_replay();

    for (;;) {
        int n = ph_queue_pop_batch(&g_ph.queue, scratch, g_ph.max_batch);
        if (n == 0) break;
        send_batch(scratch, n);
    }

    ph_mutex_lock(&g_ph.flush_lock);
    g_ph.sending = 0;
    g_ph.drain_gen++;
    ph_cond_broadcast(&g_ph.idle_cond);
    ph_mutex_unlock(&g_ph.flush_lock);
}

static void sender_main(void *arg) {
    ph_event *scratch;
    (void)arg;

    /* One heap block reused for every batch; sized once at start. */
    scratch = (ph_event *)malloc((size_t)g_ph.max_batch * sizeof(ph_event));
    if (!scratch) {
        ph_log(PH_LOG_ERROR, "sender scratch alloc failed; sender exiting");
        return;
    }

    for (;;) {
        int stop, refetch;
        ph_queue_wait(&g_ph.queue, g_ph.flush_at, g_ph.flush_interval_ms);
        ph_mutex_lock(&g_ph.flush_lock);
        refetch = g_ph.flags_refetch;
        g_ph.flags_refetch = 0;
        ph_mutex_unlock(&g_ph.flush_lock);
        if (refetch) ph__flags_fetch(); /* re-evaluate flags after ph_identify */
        drain(scratch);
        ph_mutex_lock(&g_ph.flush_lock);
        stop = g_ph.stop;
        ph_mutex_unlock(&g_ph.flush_lock);
        if (stop) break;
    }
    drain(scratch); /* final drain after the stop signal */
    free(scratch);
}

void ph__sender_start(void) {
    g_ph.stop = 0;
    g_ph.sending = 0;
    if (ph_thread_start(&g_ph.sender, sender_main, NULL) == 0) {
        g_ph.sender_running = 1;
    } else {
        g_ph.sender_running = 0;
        ph_log(PH_LOG_ERROR,
               "failed to start sender thread; events will queue but not send");
    }
}

void ph__sender_stop_and_join(void) {
    if (!g_ph.sender_running) return;
    ph_mutex_lock(&g_ph.flush_lock);
    g_ph.stop = 1;
    ph_mutex_unlock(&g_ph.flush_lock);
    ph_queue_wake(&g_ph.queue); /* break the wait so it sees stop */
    ph_thread_join(&g_ph.sender);
    g_ph.sender_running = 0;
}

void ph__sender_wake(void) { ph_queue_wake(&g_ph.queue); }

void ph__set_transport(const ph_transport *t) {
    ph_transport old;
    ph_mutex_lock(&g_ph.flush_lock);
    old = g_ph.transport;
    g_ph.transport = *t;
    ph_mutex_unlock(&g_ph.flush_lock);
    /* Safe only when no send is in flight (call right after ph_init) — the
     * previous transport may still be referenced by an active POST otherwise. */
    if (old.destroy) old.destroy(old.self);
}

void ph_flush(int timeout_ms) {
    uint64_t deadline_mono = 0;
    uint64_t gen0;
    if (!g_ph.enabled || !g_ph.sender_running) return;

    if (timeout_ms > 0)
        deadline_mono = ph_now_mono_ns() + (uint64_t)timeout_ms * 1000000ull;

    /* Snapshot the drain generation before waking, so we wait for a *full*
     * cycle to complete — which is where an offline replay happens, even when
     * the in-memory queue is already empty. */
    ph_mutex_lock(&g_ph.flush_lock);
    gen0 = g_ph.drain_gen;
    ph_mutex_unlock(&g_ph.flush_lock);

    ph__sender_wake(); /* ask the sender to drain now, not at the next interval */

    ph_mutex_lock(&g_ph.flush_lock);
    for (;;) {
        int idle = g_ph.drain_gen > gen0 && ph_queue_size(&g_ph.queue) == 0 &&
                   !g_ph.sending;
        if (idle || timeout_ms == 0) break;
        if (timeout_ms > 0) {
            uint64_t now = ph_now_mono_ns();
            int remaining;
            if (now >= deadline_mono) break;
            remaining = (int)((deadline_mono - now) / 1000000ull);
            if (remaining <= 0) remaining = 1;
            if (remaining > 100) remaining = 100; /* re-poll size periodically */
            ph_cond_timedwait(&g_ph.idle_cond, &g_ph.flush_lock, remaining);
        } else {
            ph_cond_timedwait(&g_ph.idle_cond, &g_ph.flush_lock, 100);
        }
    }
    ph_mutex_unlock(&g_ph.flush_lock);
}
