/*
 * ph_internal.h — shared internal definitions for the native backend.
 *
 * Not installed; consumers only ever see posthog.h. This declares the SDK
 * context, the packed event record that lives in the ring, and the transport
 * seam the sender thread ships batches through.
 */
#ifndef PH_INTERNAL_H
#define PH_INTERNAL_H

#include "posthog.h"
#include "ph_queue.h"
#include "ph_ratelimit.h"
#include "ph_thread.h"

#include <stdatomic.h>
#include <stdint.h>

/* --- Internal string capacities --------------------------------------- */
#define PH_API_KEY_CAP 96
#define PH_HOST_CAP 256
#define PH_RELEASE_CAP 96
#define PH_DISTINCT_ID_CAP 128

/* Group memberships scoping subsequent events via $groups. A handful is
 * plenty (PostHog's own group types are few: company/project/... — for a
 * game, "game"/"creator"). */
#ifndef PH_MAX_GROUPS
#define PH_MAX_GROUPS 4
#endif

/* Property keys stripped from every event before send (privacy denylist). */
#ifndef PH_MAX_DENYLIST
#define PH_MAX_DENYLIST 16
#endif

/* Exception payload caps. Error capture is not the product-event hot path, but
 * the structured $exception_list still stays bounded before it enters the fixed
 * event blob. The frame count is an upper bound; in practice the event blob
 * (PH_EVENT_DATA_CAP) is what limits how many frames fit — build_exception_list
 * stops early and truncates gracefully. Raise both together for deeper stacks. */
#ifndef PH_MAX_EXCEPTION_FRAMES
#define PH_MAX_EXCEPTION_FRAMES 32
#endif
#ifndef PH_EXCEPTION_FIELD_CAP
#define PH_EXCEPTION_FIELD_CAP 96
#endif

/* Feature-flag cache (remote eval, §9). */
#ifndef PH_MAX_FLAGS
#define PH_MAX_FLAGS 64
#endif
#ifndef PH_FLAG_VARIANT_CAP
#define PH_FLAG_VARIANT_CAP 64
#endif
#ifndef PH_FLAG_PAYLOAD_CAP
#define PH_FLAG_PAYLOAD_CAP 512
#endif

/* Offline persistence: batches that fail to send spill to one append-only
 * NDJSON file (one serialized batch body per line — safe because our JSON
 * never contains raw newlines) under the configured directory, capped in size
 * with drop-oldest. */
#ifndef PH_PATH_CAP
#define PH_PATH_CAP 512
#endif
#ifndef PH_OFFLINE_MAX_BYTES
#define PH_OFFLINE_MAX_BYTES (8 * 1024 * 1024)
#endif
#define PH_OFFLINE_FILENAME "ph-offline.ndjson"

/* The packed event record (ph_event) and its data[] layout live in ph_queue.h,
 * since they are the ring's element type. */

/* --- Transport seam --------------------------------------------------- */

/*
 * Out-of-band response metadata a transport may surface alongside the status.
 * The caller zero-initializes it; a transport fills what it can and leaves the
 * rest empty. Kept separate from the status so the common path stays a plain
 * int return. Currently just the raw Retry-After header (server backpressure).
 */
typedef struct ph_send_meta {
    char retry_after[64]; /* raw Retry-After header value; "" if absent */
} ph_send_meta;

/*
 * A transport ships one serialized batch body. The default is plaintext/HTTP
 * (ph_http); tests swap in a capturing mock. `send` returns an HTTP-ish status
 * (2xx => success); <0 means a transport-level failure (offline/timeout). When
 * `meta` is non-NULL the transport fills it with response metadata (e.g. the
 * Retry-After header) for the sender's rate limiter; pass NULL to ignore it.
 */
typedef struct ph_transport {
    int (*send)(void *self, const char *url, const char *body, size_t body_len,
                int timeout_ms, ph_send_meta *meta);
    /* Like send, but also captures the response body into out (NUL-terminated,
     * capped at out_cap) — used for /flags/. NULL if unsupported. */
    int (*fetch)(void *self, const char *url, const char *body, size_t body_len,
                 int timeout_ms, char *out, size_t out_cap);
    void (*destroy)(void *self);
    void *self;
} ph_transport;

/* One cached feature flag (remote eval result). */
typedef struct ph_flag {
    char key[PH_KEY_CAP];
    int enabled;
    int has_variant;
    int has_payload;
    int called_sent; /* $feature_flag_called dedup latch (per flag) */
    char variant[PH_FLAG_VARIANT_CAP];
    char payload[PH_FLAG_PAYLOAD_CAP];
} ph_flag;

/* --- SDK context (global singleton) ----------------------------------- */

typedef struct ph_ctx {
    int initialized;
    int enabled; /* master switch AND "successfully running" */

    /* Copied configuration (we never retain caller string pointers). */
    char api_key[PH_API_KEY_CAP];
    char api_host[PH_HOST_CAP];
    char release[PH_RELEASE_CAP];
    char offline_path[PH_PATH_CAP]; /* dir for the spill file; empty = memory-only */
    int person_profiles;
    int flush_at;
    int flush_interval_ms;
    int max_batch;
    int max_queue;
    int request_timeout_ms;
    int max_retries;
    int gzip;
    int send_feature_flag_events;
    ph_before_send_fn before_send;
    ph_log_fn on_log;
    void *user_data;

    /* Privacy denylist: keys stripped from every event pre-send. */
    char denylist[PH_MAX_DENYLIST][PH_KEY_CAP];
    int denylist_count;

    /* Capture-path token bucket (guarded by `lock`; refilled from the
     * monotonic clock so the hot path still reads no wall clock). */
    double rl_rate;         /* tokens per second; 0 = unlimited */
    double rl_burst;        /* bucket capacity */
    double rl_tokens;       /* current tokens */
    uint64_t rl_last_mono;  /* last refill tick */
    uint64_t rl_dropped;    /* events rejected by the limiter */

    /* Identity, super properties, and group scoping (all guarded by `lock`,
     * snapshotted into each event at capture time so the sender needs no
     * identity state of its own). */
    ph_mutex lock;
    char distinct_id[PH_DISTINCT_ID_CAP];
    int identified;
    ph_props super;
    char group_types[PH_MAX_GROUPS][PH_KEY_CAP];
    char group_keys[PH_MAX_GROUPS][PH_KEY_CAP];
    int group_count;

    /* Feature-flag cache (guarded by lock). */
    ph_flag flags[PH_MAX_FLAGS];
    int flag_count;

    /* Timing epoch: one clock reading at init; the sender reconstructs each
     * event's wall-clock time from its monotonic tick against this. */
    uint64_t epoch_wall_ns;
    uint64_t epoch_mono_ns;
    uint64_t uuid_salt;
    atomic_uint_least64_t seq;

    /* Delivery pipeline. */
    ph_queue queue;
    ph_transport transport;
    ph_ratelimit rl; /* server-directed backpressure (429/Retry-After);
                      * touched only by the sender thread */

    /* Sender thread + flush coordination. The sender parks on the queue's own
     * condition variable; flush_lock/idle_cond only coordinate the drained
     * handshake that ph_flush() blocks on. */
    ph_thread sender;
    ph_mutex flush_lock; /* guards stop, sending, sender_running */
    ph_cond idle_cond;   /* signaled when the sender drains to idle */
    int stop;            /* shutdown requested */
    int sending;         /* a batch is currently in flight */
    int sender_running;
    int flags_refetch;   /* sender should re-fetch feature flags (set on identify) */
    uint64_t drain_gen;  /* bumped after each full drain; lets ph_flush wait for
                          * a cycle to complete (e.g. an offline replay) even when
                          * the in-memory queue is already empty */
} ph_ctx;

/* The one process-global instance (§2 global singleton). */
extern ph_ctx g_ph;

/* --- Internal helpers shared across .c files -------------------------- */

/* Internal packed-stream types extending the public scalar range
 * (STR/DOUBLE/INT/BOOL = 0..3):
 *   GROUP   — key = group type, value = group key; collected into "$groups".
 *   RAWJSON — value is a pre-serialized JSON fragment emitted verbatim (used for
 *             the $exception_list payload, which the flat packer can't express).
 * Both are "non-scalar": the sender's scrub preserves them untouched while
 * before_send only sees the scalar user properties. */
#define PH_PK_GROUP 4
#define PH_PK_RAWJSON 5

/* Pack a ph_props into buf (<= cap bytes); returns bytes written. Drops
 * (does not truncate) any entry that would overflow the blob. */
size_t ph_pack_props(const ph_props *p, char *buf, size_t cap);

/* Pack a single string-valued entry (used for $groups scoping, alias, and the
 * group-event's $group_type/$group_key). Returns bytes written, 0 if it did
 * not fit. `packed_type` is PH_T_STR or PH_PK_GROUP. */
size_t ph_pack_str_entry(char *buf, size_t cap, unsigned char packed_type,
                         const char *key, const char *val);

/* Iterate a packed blob. *cur points at the next entry; end bounds the blob.
 * Returns 1 (advancing *cur, filling the out-params, which alias into the blob)
 * or 0 at end / on a malformed tail. The one reader used by both the serializer
 * and the scrub/unpack path so the binary format lives in exactly one place. */
int ph_blob_next(const char **cur, const char *end, unsigned char *type,
                 const char **key, size_t *klen, const char **val, size_t *vlen);

/* Reconstruct the scalar (STR/DOUBLE/INT/BOOL) entries of a packed blob into
 * `out`, skipping non-scalar entries (groups, rawjson). Used by the sender's
 * scrub stage so before_send can see props as a ph_props. */
void ph_unpack_props(const char *blob, size_t len, ph_props *out);

/* Serialize `events` into a /batch/ envelope body appended to `out`.
 * Pure and side-effect-free: no threads, no network — this is the
 * parity-critical piece and is unit-tested directly. */
struct ph_strbuf;
void ph_serialize_batch(const ph_ctx *ctx, const ph_event *events, int n,
                        struct ph_strbuf *out);

/* Serialize a ph_props to a JSON object string; shared with the WASM backend
 * so property JSON matches byte-for-byte across native and wasm. */
void ph_serialize_props_object(const ph_props *p, struct ph_strbuf *out);

/* Log through the configured sink, if any. */
void ph_log(ph_log_level level, const char *fmt, ...);

/* Test/embedding hook: install a transport, replacing the default. Takes
 * ownership (calls the previous transport's destroy). */
void ph__set_transport(const ph_transport *t);

/* Native backend entry points (ph_native.c) used by ph_core.c. */
void ph__sender_start(void);
void ph__sender_stop_and_join(void);
void ph__sender_wake(void);

/* Feature flags (ph_flags.c). ingest parses a /flags/ response into the cache;
 * fetch does the network round-trip then ingest; the accessors read the cache
 * and (via ph__emit_ff_called) emit a deduped $feature_flag_called. */
void ph__flags_ingest(const char *json, size_t len);
void ph__flags_fetch(void);
int ph__flags_is_enabled(const char *key, int fallback);
ph_result ph__flags_get(const char *key, char *out, int cap);
ph_result ph__flags_get_payload(const char *key, char *out, int cap);

/* Emit a deduped $feature_flag_called event (defined in ph_core.c). */
void ph__emit_ff_called(const char *key, const char *value);

#endif /* PH_INTERNAL_H */
