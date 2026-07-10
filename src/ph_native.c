/*
 * ph_native.c - the background sender thread and flush coordination.
 *
 * capture() only ever touches the ring (ph_core.c). Everything expensive for
 * product events - serialization, before_send, the network POST - happens here,
 * off the caller thread. Exception events pre-scrub their structured payload
 * before enqueue so raw exception text can be redacted. The loop parks on the
 * queue's condition variable until it has
 * flush_at events or flush_interval_ms elapses, then drains everything present
 * in max_batch-sized POSTs.
 */
#include "ph_internal.h"
#include "ph_jsonval.h"
#include "ph_str.h"
#include "ph_time.h"
#include "ph_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* api_host + "/batch/", tolerating a trailing slash on the host. */
static void build_batch_url(char *out, size_t cap) {
    size_t n = strlen(g_ph.api_host);
    while (n > 0 && g_ph.api_host[n - 1] == '/') n--;
    snprintf(out, cap, "%.*s/batch/", (int)n, g_ph.api_host);
}

/* POST one already-serialized batch body. Fills `meta` (Retry-After, ...) when
 * non-NULL. Returns the HTTP-ish status. */
static int post_body(const char *body, size_t len, ph_send_meta *meta) {
    char url[PH_HOST_CAP + 16];
    ph_transport t;
    build_batch_url(url, sizeof(url));
    ph_mutex_lock(&g_ph.flush_lock);
    t = g_ph.transport;
    ph_mutex_unlock(&g_ph.flush_lock);
    if (!t.send) return -1;
    return t.send(t.self, url, body, len, g_ph.request_timeout_ms, meta);
}

/* PostHog signals event quota as a 2xx body: {"status":1,"quota_limited":[...]}.
 * Returns 1 when that list names our resource ("events"), so the sender holds
 * like a 429. Deliberately narrow: an absent field or an unrelated resource
 * (e.g. "recordings") leaves us sending. */
static int quota_limited_events(const char *body) {
    return body && body[0] && strstr(body, "quota_limited") != NULL &&
           strstr(body, "\"events\"") != NULL;
}

/* POST a batch and fold any server backpressure into the rate limiter, so the
 * next drain holds off instead of hammering a throttled endpoint: an HTTP 429
 * (or 503 with Retry-After), or a 2xx carrying PostHog's quota-limit notice.
 * Returns the HTTP-ish status. */
static int post_and_note(const char *body, size_t len) {
    ph_send_meta meta;
    int status;
    memset(&meta, 0, sizeof meta);
    status = post_body(body, len, &meta);
    if (status == 429 || status == 503)
        ph_ratelimit_note_response(&g_ph.rl, status, meta.retry_after,
                                   ph_now_mono_ns(), ph_now_wall_ns());
    else if (status >= 200 && status < 300 && quota_limited_events(meta.body))
        ph_ratelimit_arm(&g_ph.rl, PH_RL_DEFAULT_BACKOFF_MS, ph_now_mono_ns());
    return status;
}

/* --- Offline spill file ----------------------------------------------
 *
 * When a send fails and offline_path is set, the serialized batch is appended
 * as one line to a single NDJSON file. On the next drain the sender replays
 * the file (oldest first) before new memory events, so an app that runs
 * offline for a long stretch loses nothing when it reconnects.
 */

static int ensure_offline_dir(void) {
#if defined(_WIN32)
    return _mkdir(g_ph.offline_path) == 0 || errno == EEXIST;
#else
    return mkdir(g_ph.offline_path, 0700) == 0 || errno == EEXIST;
#endif
}

static FILE *open_private_file(const char *path, int append) {
#if defined(_WIN32)
    int flags = _O_WRONLY | _O_CREAT | _O_BINARY |
                (append ? _O_APPEND : _O_TRUNC);
    int fd = _open(path, flags, _S_IREAD | _S_IWRITE);
    FILE *f;
    if (fd < 0) return NULL;
    f = _fdopen(fd, append ? "ab" : "wb");
    if (!f) _close(fd);
    return f;
#else
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    int fd = open(path, flags, 0600);
    FILE *f;
    if (fd < 0) return NULL;
    (void)fchmod(fd, 0600);
    f = fdopen(fd, append ? "ab" : "wb");
    if (!f) close(fd);
    return f;
#endif
}

static int durable_close(FILE *f) {
    int ok = fflush(f) == 0;
    if (ok) {
#if defined(_WIN32)
        ok = _commit(_fileno(f)) == 0;
#else
        ok = fsync(fileno(f)) == 0;
#endif
    }
    if (fclose(f) != 0) ok = 0;
    return ok;
}

static int atomic_replace(const char *tmp, const char *path) {
#if defined(_WIN32)
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING |
                                  MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp, path) == 0;
#endif
}

static int write_atomic(const char *path, const char *data, size_t len) {
    char tmp[PH_PATH_CAP + 48];
    FILE *f;
    int ok;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = open_private_file(tmp, 0);
    if (!f) return 0;
    ok = len == 0 || fwrite(data, 1, len, f) == len;
    if (!durable_close(f)) ok = 0;
    if (ok) ok = atomic_replace(tmp, path);
    if (!ok) remove(tmp);
    return ok;
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

static int batch_event_count(const char *body, size_t len) {
    ph_jv *root = ph_jv_parse(body, len);
    const ph_jv *batch;
    int n = 0;
    if (!root) return 0;
    batch = ph_jv_get(root, "batch");
    if (ph_jv_type_of(batch) == PH_JV_ARR) n = ph_jv_len(batch);
    ph_jv_free(root);
    return n;
}

static uint64_t event_count_in_lines(const char *buf, size_t len) {
    size_t start = 0, i;
    uint64_t count = 0;
    for (i = 0; i < len; i++) {
        if (buf[i] != '\n') continue;
        if (i > start) count += (uint64_t)batch_event_count(buf + start, i - start);
        start = i + 1;
    }
    return count;
}

/* Keep the spill file under the byte cap by dropping the oldest whole lines. */
static int offline_enforce_cap(const char *path, size_t incoming) {
    long sz = file_size(path);
    char *buf;
    size_t rd, target, drop;
    FILE *f;
    uint64_t dropped_events = 0;

    if (incoming >= (size_t)PH_OFFLINE_MAX_BYTES) return 0;
    if (sz < 0) return 1;
    if ((size_t)sz + incoming + 1 <= (size_t)PH_OFFLINE_MAX_BYTES) return 1;

    f = fopen(path, "rb");
    if (!f) return 0;
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        free(buf);
        return 0;
    }

    /* Keep at most (cap - incoming) newest bytes, aligned to a line boundary. */
    target = (size_t)PH_OFFLINE_MAX_BYTES > incoming + 1
                 ? (size_t)PH_OFFLINE_MAX_BYTES - incoming - 1
                 : 0;
    drop = rd > target ? rd - target : 0;
    while (drop < rd && buf[drop] != '\n') drop++;
    if (drop < rd) drop++; /* step past the newline */

    if (drop > 0) dropped_events = event_count_in_lines(buf, drop);
    if (!write_atomic(path, rd > drop ? buf + drop : "", rd - drop)) {
        free(buf);
        return 0;
    }
    if (dropped_events)
        atomic_fetch_add(&g_ph.st_failed, (uint_least64_t)dropped_events);
    free(buf);
    return 1;
}

static int offline_spill(const char *body, size_t len) {
    char path[PH_PATH_CAP + 32];
    FILE *f;
    int ok;
    if (!g_ph.offline_path[0] || len == 0) return 0;
    if (!ensure_offline_dir()) return 0;
    offline_spill_path(path, sizeof(path));
    if (!offline_enforce_cap(path, len)) return 0;
    f = open_private_file(path, 1);
    if (!f) {
        ph_log(PH_LOG_WARN, "offline: cannot open spill file %s", path);
        return 0;
    }
    /* Only newline-terminate a fully-written record; a short write leaves the
     * partial unterminated so offline_replay's torn-tail guard drops it. */
    ok = fwrite(body, 1, len, f) == len && fputc('\n', f) != EOF;
    if (!ok)
        ph_log(PH_LOG_WARN, "offline: short write spilling to %s", path);
    if (!durable_close(f)) ok = 0;
    return ok;
}

/* A deterministic client error - a 4xx other than 429 backpressure or 408
 * timeout. Retrying/persisting deterministic rejects just fails again forever,
 * so drop those rather than spill/keep and block the queue behind them. */
static int is_permanent_reject(int status) {
    return status >= 400 && status < 500 && status != 408 && status != 429;
}

/* Replay spilled batches oldest-first. A 2xx is accepted and dropped from the
 * spill - a quota_limited 200 still accepted the request (its events were
 * server-dropped), so re-sending won't recover them, matching how send_batch
 * treats a delivered batch. Stop replaying on the first transient failure, or
 * once a response arms the rate limiter, keeping the untried remainder so order
 * is preserved and we don't hammer a throttle. */
static void offline_replay(void) {
    char path[PH_PATH_CAP + 32];
    long sz;
    FILE *f;
    char *buf;
    size_t rd, i, line_start;
    int stopped = 0;
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
    for (i = 0; i < rd; i++) {
        if (buf[i] != '\n') continue;
        if (i > line_start) {
            size_t linelen = i - line_start;
            int events = batch_event_count(buf + line_start, linelen);
            int keep_line;
            if (stopped) {
                keep_line = 1; /* never attempted - keep for a later drain */
            } else if (g_ph.max_batch_bytes > 0 &&
                       linelen > (size_t)g_ph.max_batch_bytes) {
                keep_line = 0;
                if (events > 0)
                    atomic_fetch_add(&g_ph.st_failed, (uint_least64_t)events);
                ph_log(PH_LOG_WARN,
                       "offline: dropping %lu-byte batch over max_batch_bytes",
                       (unsigned long)linelen);
            } else {
                int status = post_and_note(buf + line_start, linelen);
                if (status >= 200 && status < 300) {
                    keep_line = 0; /* accepted (even a quota 200) - drop it */
                    if (events > 0)
                        atomic_fetch_add(&g_ph.st_sent, (uint_least64_t)events);
                    if (ph_ratelimit_blocked(&g_ph.rl, ph_now_mono_ns()))
                        stopped = 1; /* ...but hold the untried remainder */
                } else if (is_permanent_reject(status)) {
                    keep_line = 0; /* client error - drop, don't block the queue */
                    if (events > 0)
                        atomic_fetch_add(&g_ph.st_failed, (uint_least64_t)events);
                    ph_log(PH_LOG_WARN,
                           "offline: dropping batch rejected with status %d", status);
                } else {
                    keep_line = 1; /* transient failure - keep and stop */
                    stopped = 1;
                }
            }
            if (keep_line) {
                ph_strbuf_append(&keep, buf + line_start, linelen);
                ph_strbuf_append_char(&keep, '\n');
            }
        }
        line_start = i + 1;
    }
    /* offline_spill newline-terminates every record, so any bytes past the last
     * '\n' are a torn write from a process killed mid-spill. Drop them (never
     * copied into `keep`, so the rewrite below discards them): a partial batch
     * would 400 on every run and poison the queue forever. */
    if (line_start < rd)
        ph_log(PH_LOG_WARN, "offline: dropping torn %lu-byte trailing spill record",
               (unsigned long)(rd - line_start));
    free(buf);

    if (keep.len == 0) {
        if (remove(path) != 0 && errno != ENOENT)
            ph_log(PH_LOG_WARN, "offline: cannot remove drained spill %s; "
                                "delivered records may re-send", path);
    } else {
        if (!write_atomic(path, keep.data, keep.len))
            ph_log(PH_LOG_WARN, "offline: cannot atomically rewrite spill %s; "
                                "delivered records may re-send", path);
    }
    ph_strbuf_free(&keep);
}

/* remove-key lives in ph_util.c (ph_props_remove_key), shared across backends. */

/* Scrub one event: strip denylisted keys, and (when run_before_send is set) run
 * before_send. Returns 1 to keep (blob rewritten in place) or 0 to drop.
 * Non-scalar entries ($groups, $exception_list) are preserved untouched. */
static int scrub_one(ph_event *e, int run_before_send) {
    char name[PH_EVENT_NAME_CAP];
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

    for (i = 0; i < g_ph.denylist_count; i++) ph_props_remove_key(&props, g_ph.denylist[i]);
    if (run_before_send && g_ph.before_send) {
        int keep;
        ph__in_callback++;
        keep = g_ph.before_send(name, &props, g_ph.user_data);
        ph__in_callback--;
        if (!keep) {
            atomic_fetch_add(&g_ph.st_before_send_dropped, (uint_least64_t)1);
            return 0;
        }
    }

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
        /* Exceptions pre-scrub at capture (before_send already ran), but the
         * merged super props are stamped afterward - re-apply the denylist
         * without invoking the hook twice. Every other event kind contains
         * caller-controlled values and goes through the full privacy pass. */
        if (evs[i].flags & PH_EVF_SCRUBBED) {
            if (g_ph.denylist_count > 0) scrub_one(&evs[i], 0);
        } else if (!scrub_one(&evs[i], 1)) {
            continue; /* dropped by before_send */
        }
        if (out != i) evs[out] = evs[i];
        out++;
    }
    return out;
}

/* Sleep up to `ms` in small chunks, returning 1 the moment shutdown is
 * requested - so retry backoff never delays ph_shutdown by the full schedule. */
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

static void persist_run(ph_event *evs, int n);

static void spill_batch(ph_event *evs, int n) {
    n = scrub_events(evs, n);
    if (n == 0) return;
    persist_run(evs, n);
}

static void spill_queued(ph_event *scratch) {
    int spilled = 0;
    for (;;) {
        int n = ph_queue_pop_batch(&g_ph.queue, scratch, g_ph.max_batch);
        if (n == 0) break;
        if (g_ph.offline_path[0]) {
            spill_batch(scratch, n);
        } else {
            spilled += n;
        }
    }
    if (spilled > 0)
        ph_log(PH_LOG_WARN, "rate-limited shutdown dropped %d held events "
               "(set offline_path to persist)", spilled);
}

/* Exponential backoff between batch-POST retries: PH_RETRY_BASE_MS << attempt,
 * capped at PH_RETRY_MAX_MS. */
#ifndef PH_RETRY_BASE_MS
#define PH_RETRY_BASE_MS 100
#endif
#ifndef PH_RETRY_MAX_MS
#define PH_RETRY_MAX_MS 2000
#endif

/* POST one already-serialized body, retrying failures a retry can plausibly fix:
 * 5xx, 408/timeouts, and network errors (status < 0). 2xx delivered; a
 * deterministic 4xx reject (bad key/body) won't change; a 429 (or a 503 carrying
 * Retry-After) is server backpressure - post_and_note arms the limiter and we
 * stop rather than hammer the throttle. Exponential backoff between attempts,
 * interruptible on shutdown. `n` is the event count, for diagnostics + the
 * delivery counters. On permanent failure the body spills (if offline_path is
 * set) or is dropped. */
static int send_one(const char *body, size_t len, int n) {
    int status = -1;
    int attempt, attempts;

    attempts = g_ph.max_retries >= 0 ? g_ph.max_retries + 1 : 1;
    for (attempt = 0; attempt < attempts; attempt++) {
        if (attempt > 0) {
            int shift = attempt - 1 > 6 ? 6 : attempt - 1; /* clamp: avoid overflow */
            int backoff = PH_RETRY_BASE_MS << shift;
            atomic_fetch_add(&g_ph.st_retries, (uint_least64_t)1);
            if (backoff > PH_RETRY_MAX_MS) backoff = PH_RETRY_MAX_MS;
            if (sender_backoff_wait(backoff)) break; /* shutdown requested */
        }
        status = post_and_note(body, len);
        if (status >= 200 && status < 300) break; /* delivered */
        if (is_permanent_reject(status)) break; /* client error: won't change */
        if (ph_ratelimit_blocked(&g_ph.rl, ph_now_mono_ns())) break; /* backpressure */
    }
    if (status >= 200 && status < 300) {
        atomic_fetch_add(&g_ph.st_sent, (uint_least64_t)n);
    } else if (is_permanent_reject(status)) {
        /* Deterministic client error (bad key/body): spilling would just re-POST
         * and fail on every drain, blocking the queue. Drop it. */
        atomic_fetch_add(&g_ph.st_failed, (uint_least64_t)n);
        ph_log(PH_LOG_WARN, "batch rejected (status %d); %d events dropped "
                            "(client error, will not retry)", status, n);
    } else if (g_ph.offline_path[0]) {
        if (offline_spill(body, len)) {
            ph_log(PH_LOG_INFO, "batch spilled to offline queue (status %d)", status);
        } else {
            atomic_fetch_add(&g_ph.st_failed, (uint_least64_t)n);
            ph_log(PH_LOG_ERROR, "offline spill failed; %d events lost (status %d)",
                   n, status);
        }
    } else {
        atomic_fetch_add(&g_ph.st_failed, (uint_least64_t)n);
        ph_log(PH_LOG_WARN,
               "batch POST failed (status %d); %d events dropped "
               "(set offline_path to persist failed batches)", status, n);
    }
    return ph_ratelimit_blocked(&g_ph.rl, ph_now_mono_ns());
}

/* Persist an already-scrubbed run without exceeding max_batch_bytes. Used for
 * shutdown spill and for unsent siblings after one split POST trips server
 * backpressure. Without offline_path the run is accounted as failed. */
static void persist_run(ph_event *evs, int n) {
    ph_strbuf body;

    if (!g_ph.offline_path[0]) {
        atomic_fetch_add(&g_ph.st_failed, (uint_least64_t)n);
        ph_log(PH_LOG_WARN, "%d unsent events dropped (set offline_path to persist)", n);
        return;
    }

    ph_strbuf_init(&body);
    ph_serialize_batch(&g_ph, evs, n, &body);
    if (body.oom) {
        ph_strbuf_free(&body);
        atomic_fetch_add(&g_ph.st_failed, (uint_least64_t)n);
        ph_log(PH_LOG_ERROR, "serialize OOM; dropped %d held events", n);
        return;
    }
    if (g_ph.max_batch_bytes > 0 && body.len > (size_t)g_ph.max_batch_bytes) {
        if (n > 1) {
            int half = n / 2;
            ph_strbuf_free(&body);
            persist_run(evs, half);
            persist_run(evs + half, n - half);
            return;
        }
        ph_strbuf_free(&body);
        atomic_fetch_add(&g_ph.st_failed, (uint_least64_t)1);
        ph_log(PH_LOG_WARN,
               "single event exceeds max_batch_bytes; event dropped instead of spilled");
        return;
    }
    if (!offline_spill(body.data ? body.data : "", body.len)) {
        atomic_fetch_add(&g_ph.st_failed, (uint_least64_t)n);
        ph_log(PH_LOG_ERROR, "offline spill failed; %d held events lost", n);
    }
    ph_strbuf_free(&body);
}

/* Serialize evs[0..n) (already scrubbed) and POST it. If the serialized body
 * would exceed max_batch_bytes, split the run in half and recurse so no single
 * POST exceeds the cap - PostHog rejects oversized bodies. A lone event over the
 * cap is dropped because it cannot be split. Returns nonzero when a response
 * arms server backpressure; recursive callers persist their unsent siblings
 * without issuing another POST. */
static int send_run(ph_event *evs, int n) {
    ph_strbuf body;
    int blocked;

    ph_strbuf_init(&body);
    ph_serialize_batch(&g_ph, evs, n, &body);
    if (body.oom) {
        ph_strbuf_free(&body);
        atomic_fetch_add(&g_ph.st_failed, (uint_least64_t)n);
        ph_log(PH_LOG_ERROR, "serialize OOM; dropped %d events", n);
        return 0;
    }
    if (g_ph.max_batch_bytes > 0 && body.len > (size_t)g_ph.max_batch_bytes) {
        if (n > 1) {
            int half = n / 2;
            ph_strbuf_free(&body);
            if (send_run(evs, half)) {
                persist_run(evs + half, n - half);
                return 1;
            }
            return send_run(evs + half, n - half);
        }
        ph_strbuf_free(&body);
        atomic_fetch_add(&g_ph.st_failed, (uint_least64_t)1);
        ph_log(PH_LOG_WARN,
               "single event exceeds max_batch_bytes; event dropped instead of sent");
        return 0;
    }
    blocked = send_one(body.data ? body.data : "", body.len, n);
    ph_strbuf_free(&body);
    return blocked;
}

static void send_batch(ph_event *evs, int n) {
    n = scrub_events(evs, n); /* strip denylist / run before_send, compacting drops */
    if (n == 0) return;       /* every event was scrubbed away */
    (void)send_run(evs, n);
}

/* Drain: replay any spilled batches first (preserving order across a
 * reconnect), then send everything currently in memory, in max_batch chunks.
 * `sending` covers the whole pass so ph_flush() waits for the replay too. */
static void drain(ph_event *scratch, int final) {
    ph_mutex_lock(&g_ph.flush_lock);
    g_ph.sending = 1;
    ph_mutex_unlock(&g_ph.flush_lock);

    /* Under server backpressure (429/Retry-After) hold everything: no replay,
     * no sends. Events stay in the bounded ring (drop-oldest on overflow), so
     * we neither block the caller nor waste a request against the throttle. */
    if (!ph_ratelimit_blocked(&g_ph.rl, ph_now_mono_ns())) {
        offline_replay();
        for (;;) {
            int n = ph_queue_pop_batch(&g_ph.queue, scratch, g_ph.max_batch);
            if (n == 0) break;
            send_batch(scratch, n);
            /* A batch just tripped the limiter - stop, hold the rest queued. */
            if (ph_ratelimit_blocked(&g_ph.rl, ph_now_mono_ns())) break;
        }
    } else if (final) {
        /* Respect server backpressure on shutdown. If the host provided an
         * offline queue, persist held in-memory events instead of freeing them
         * with the queue; otherwise they are dropped with a diagnostic. */
        spill_queued(scratch);
    }

    ph_mutex_lock(&g_ph.flush_lock);
    g_ph.sending = 0;
    g_ph.drain_gen++;
    ph_cond_broadcast(&g_ph.idle_cond);
    ph_mutex_unlock(&g_ph.flush_lock);
}

/* Build the on_stats health snapshot and hand it to the callback. Sender-thread
 * only. Compact single-line JSON: queue depth + cap, delivered/failed/retried,
 * and drops broken down by reason. Numbers only, so a fixed stack buffer fits. */
static void emit_stats(void) {
    char buf[320];
    uint64_t queued, rl_dropped, ring_dropped, sent, failed, retries, bs, total;
    int len;

    if (!g_ph.on_stats) return;
    queued = (uint64_t)ph_queue_size(&g_ph.queue);
    ring_dropped = ph_queue_dropped(&g_ph.queue);
    ph_mutex_lock(&g_ph.lock);
    rl_dropped = g_ph.rl_dropped;
    ph_mutex_unlock(&g_ph.lock);
    sent = atomic_load(&g_ph.st_sent);
    failed = atomic_load(&g_ph.st_failed);
    retries = atomic_load(&g_ph.st_retries);
    bs = atomic_load(&g_ph.st_before_send_dropped);
    total = rl_dropped + ring_dropped + bs;

    len = snprintf(buf, sizeof(buf),
        "{\"queued\":%llu,\"queue_cap\":%d,\"sent\":%llu,\"failed\":%llu,"
        "\"retries\":%llu,\"dropped\":{\"total\":%llu,\"rate_limited\":%llu,"
        "\"queue_overflow\":%llu,\"before_send\":%llu}}",
        (unsigned long long)queued, g_ph.max_queue,
        (unsigned long long)sent, (unsigned long long)failed,
        (unsigned long long)retries, (unsigned long long)total,
        (unsigned long long)rl_dropped, (unsigned long long)ring_dropped,
        (unsigned long long)bs);
    if (len > 0) {
        ph__in_callback++;
        g_ph.on_stats(buf, (size_t)len, g_ph.user_data);
        ph__in_callback--;
    }
}

static void sender_main(void *arg) {
    ph_event *scratch = (ph_event *)arg;
    uint64_t last_stats;
    last_stats = ph_now_mono_ns();

    for (;;) {
        int stop, refetch;
        /* Wake at least as often as the stats interval so on_stats fires on time
         * even while the queue is idle (otherwise we only wake every flush). */
        int wait_ms = g_ph.flush_interval_ms;
        if (g_ph.stats_interval_ms > 0 && g_ph.stats_interval_ms < wait_ms)
            wait_ms = g_ph.stats_interval_ms;
        ph_queue_wait(&g_ph.queue, g_ph.flush_at, wait_ms);
        ph_mutex_lock(&g_ph.flush_lock);
        refetch = g_ph.flags_refetch;
        g_ph.flags_refetch = 0;
        ph_mutex_unlock(&g_ph.flush_lock);
        if (refetch) ph__flags_fetch(); /* re-evaluate flags after ph_identify */
        drain(scratch, 0);
        if (g_ph.on_stats && g_ph.stats_interval_ms > 0) {
            uint64_t now = ph_now_mono_ns();
            if (now - last_stats >= (uint64_t)g_ph.stats_interval_ms * 1000000ull) {
                emit_stats();
                last_stats = now;
            }
        }
        ph_mutex_lock(&g_ph.flush_lock);
        stop = g_ph.stop;
        ph_mutex_unlock(&g_ph.flush_lock);
        if (stop) break;
        /* If the server throttled us, park until the window clears rather than
         * waking every interval only for drain() to skip an already-full queue.
         * Interruptible on shutdown. */
        {
            uint64_t rem = ph_ratelimit_remaining_ms(&g_ph.rl, ph_now_mono_ns());
            if (rem > 0) {
                int ms = rem > 2000000000u ? 2000000000 : (int)rem;
                if (sender_backoff_wait(ms)) break;
            }
        }
    }
    drain(scratch, 1); /* final drain after the stop signal */
    free(scratch);
}

int ph__sender_start(void) {
    ph_event *scratch;
    g_ph.stop = 0;
    g_ph.sending = 0;
    ph_ratelimit_init(&g_ph.rl);
    if ((size_t)g_ph.max_batch > (size_t)-1 / sizeof(ph_event)) return -1;
    scratch = (ph_event *)malloc((size_t)g_ph.max_batch * sizeof(ph_event));
    if (!scratch) return -1;
    if (ph_thread_start(&g_ph.sender, sender_main, scratch) == 0) {
        g_ph.sender_running = 1;
        return 0;
    } else {
        free(scratch);
        g_ph.sender_running = 0;
        return -1;
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
    /* Safe only when no send is in flight (call right after ph_init) - the
     * previous transport may still be referenced by an active POST otherwise. */
    if (old.destroy) old.destroy(old.self);
}

void ph_flush(int timeout_ms) {
    uint64_t deadline_mono = 0;
    uint64_t gen0;
    if (!g_ph.enabled || !g_ph.sender_running) return;
    if (ph__in_callback) return;
    /* Callbacks run on the sender. Waiting for the sender from itself would
     * deadlock; treat callback-initiated flush as a safe no-op. */
    if (ph_thread_is_current(&g_ph.sender)) return;

    if (timeout_ms > 0)
        deadline_mono = ph_now_mono_ns() + (uint64_t)timeout_ms * 1000000ull;

    /* Snapshot the drain generation before waking, so we wait for a *full*
     * cycle to complete - which is where an offline replay happens, even when
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
