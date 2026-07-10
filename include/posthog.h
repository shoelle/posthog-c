/*
 * posthog.h - PostHog C SDK, public C API.
 *
 * A small, embeddable PostHog client for C/C++ applications. One C interface,
 * two compile-time transports: `native` (owns HTTP + a background sender
 * thread + an on-disk offline queue) and `wasm` (a thin shim over the
 * browser's already-loaded `window.posthog`). Callers never see the split.
 *
 * Everything rides PostHog's documented raw ingestion API (`/batch/`,
 * `/i/v0/e/`, `/flags/`) - no dependency on another PostHog SDK.
 *
 * This is a C header: any C or C++ program can include it, and any language
 * can bind the resulting interface. A header-only C++ convenience wrapper lives in
 * posthog.hpp.
 *
 * Design rationale: see DESIGN.md.
 * Threading: after ph_init(), every public function is safe to call from any
 * thread. On native, capture() copies into a bounded queue and returns; all
 * network I/O happens on a background sender thread.
 */
#ifndef POSTHOG_H
#define POSTHOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Version ---------------------------------------------------------- */

#define PH_VERSION_MAJOR 0
#define PH_VERSION_MINOR 7
#define PH_VERSION_PATCH 0
#define PH_VERSION_STRING "0.7.0"

/* Returns the compiled-in library version, e.g. "0.7.0". */
const char *ph_version(void);

/* --- Fixed public capacities -----------------------------------------
 *
 * These values are part of the public ph_prop/ph_props ABI and must match the
 * compiled library, so they are deliberately not caller-overridable. Fork the
 * SDK and change both the library and header together if an application needs a
 * different ABI. Internal queue/blob capacities remain private implementation
 * details.
 */
#if defined(PH_MAX_PROPS) || defined(PH_KEY_CAP) || defined(PH_VAL_CAP) || \
    defined(PH_MAX_DENYLIST) || defined(PH_DISTINCT_ID_CAP) || \
    defined(PH_EVENT_NAME_CAP)
#error "posthog-c public capacities are fixed ABI constants; do not override them"
#endif
#define PH_MAX_PROPS 24 /* max user properties per event */
#define PH_KEY_CAP 48 /* max property-key bytes, incl. NUL */
#define PH_VAL_CAP 192 /* max string-value bytes, incl. NUL */
#define PH_MAX_DENYLIST 16 /* max property_denylist keys honored (per backend) */
#define PH_DISTINCT_ID_CAP 128 /* max distinct-id bytes, incl. NUL */
#define PH_EVENT_NAME_CAP 128 /* max event-name bytes, incl. NUL */

/* --- Result codes ----------------------------------------------------- */

typedef enum ph_result {
    PH_OK = 0,             /* success */
    PH_ERR = -1,           /* generic failure */
    PH_ERR_DISABLED = -2,  /* SDK not enabled / not initialized */
    PH_ERR_FULL = -3,      /* fixed buffer full; item dropped */
    PH_ERR_BADARG = -4,    /* NULL or invalid argument */
    PH_ERR_TRUNCATED = -5,    /* stored, but a key/value was truncated to fit */
    PH_ERR_RATE_LIMITED = -6  /* capture rejected by the token-bucket rate limiter */
} ph_result;

/* --- Person-profile policy --------------------------------------
 *
 * Controls whether an event builds a PostHog person profile. Anonymous
 * events are ~4x cheaper and the right default for un-signed-in users.
 */
typedef enum ph_person_profiles {
    PH_IDENTIFIED_ONLY = 0, /* default: anonymous until ph_identify() */
    PH_ALWAYS = 1,
    PH_NEVER = 2
} ph_person_profiles;

/* --- Diagnostics log levels ------------------------------------------- */

typedef enum ph_log_level {
    PH_LOG_ERROR = 0,
    PH_LOG_WARN = 1,
    PH_LOG_INFO = 2,
    PH_LOG_DEBUG = 3
} ph_log_level;

/* --- Property set ------------------------------------------------
 *
 * A bounded, inline, POD property set. Build one on the stack, fill it with
 * the typed setters, and hand it to ph_capture(); the SDK copies what it
 * needs before returning, so no caller memory is borrowed and nothing is
 * heap-allocated on the caller thread. Reserved keys ($set, $groups, ...)
 * are written by dedicated helpers (ph_identify, ph_group), never by hand,
 * so the JSON shape cannot be gotten wrong.
 */
typedef enum ph_prop_type {
    PH_T_STR = 0,
    PH_T_DOUBLE = 1,
    PH_T_INT = 2,
    PH_T_BOOL = 3
} ph_prop_type;

typedef struct ph_prop {
    char key[PH_KEY_CAP];
    unsigned char type; /* ph_prop_type */
    union {
        char str[PH_VAL_CAP];
        double dbl;
        int64_t i64;
        int boolean;
    } val;
} ph_prop;

typedef struct ph_props {
    int count;   /* number of populated items */
    int dropped; /* items rejected because the set was full */
    ph_prop items[PH_MAX_PROPS];
} ph_props;

/* Reset a property set to empty. Always call before first use. */
void ph_props_init(ph_props *p);

/*
 * Typed setters. Each appends one key/value. Returns:
 *   PH_OK             stored cleanly
 *   PH_ERR_TRUNCATED  stored, but key or value was truncated to its cap
 *   PH_ERR_FULL       set already holds PH_MAX_PROPS items; not stored
 *   PH_ERR_BADARG     p or key was NULL/empty; not stored
 * A duplicate key appends a second entry (last-wins at serialization).
 * Callers typically ignore the return value.
 */
ph_result ph_props_set_str(ph_props *p, const char *key, const char *val);
ph_result ph_props_set_double(ph_props *p, const char *key, double val);
ph_result ph_props_set_int(ph_props *p, const char *key, int64_t val);
ph_result ph_props_set_bool(ph_props *p, const char *key, int val);

/* --- before_send scrubber --------------------------------------
 *
 * Runs before serialization. Native product events run on the sender thread;
 * exception events run before enqueue so structured exception text can be
 * redacted before $exception_list is built. Mutate `props` in place to redact,
 * or return 0 to drop the event entirely. Return nonzero to keep it. `event`
 * is the event name. `user` is ph_config.user_data. NULL hook = pass-through.
 */
typedef int (*ph_before_send_fn)(const char *event, ph_props *props, void *user);

/* Optional diagnostics sink for drops/retries/HTTP errors. NULL = silent. */
typedef void (*ph_log_fn)(ph_log_level level, const char *msg, void *user);

/* Optional periodic health snapshot. When ph_config.stats_interval_ms > 0, the
 * sender thread calls this every ~stats_interval_ms with a compact JSON blob:
 * queue depth, delivered/failed/retried counts, and drops broken down by reason
 * (rate_limited / queue_overflow / before_send). `json` is valid only for the
 * duration of the call. Fired on the sender thread, never the caller thread. */
typedef void (*ph_stats_fn)(const char *json, size_t len, void *user);

/* --- Configuration ----------------------------------------------
 *
 * POD config passed to ph_init(). Zero-initialize it (memset or {0}) and set
 * only what you need; ph_config_defaults() fills sensible defaults for the
 * numeric/policy fields. Only api_key and distinct_id are strictly required.
 * String fields are copied by ph_init(); you keep ownership of your buffers.
 */
typedef struct ph_config {
    const char *api_key;   /* phc_... project (write-only) key */
    const char *api_host;  /* "https://us.i.posthog.com" or your /ingest proxy */
    const char *distinct_id; /* anonymous install id; replace via ph_identify */

    int flush_at;           /* queue depth that triggers a send (default 20) */
    int flush_interval_ms;  /* max wait before a partial flush (default 30000) */
    int max_batch;          /* events per POST (default 50) */
    int max_queue;          /* drop-oldest ring cap (default 1000) */
    int request_timeout_ms; /* per-POST timeout (default 10000) */
    int max_retries;        /* per-batch retries (exp backoff; 5xx/timeout only)
                             * before spill/drop (default 3) */
    int gzip;               /* gzip /batch/ bodies with Content-Encoding: gzip
                             * (default 1 = on; native only, wasm ignores it) */

    const char *offline_path; /* dir for the on-disk spill queue; NULL = memory-only */
    const char *release;      /* e.g. "myapp@1.2.3" - tags every event */

    int enabled;         /* master switch; 0 => every call is a no-op */
    int person_profiles; /* ph_person_profiles (default PH_IDENTIFIED_ONLY) */

    int send_feature_flag_events; /* emit $feature_flag_called on flag reads (default 1) */
    int preload_flags;            /* fetch flags during ph_init (default 1; v0.7) */

    ph_before_send_fn before_send; /* scrubber; NULL = pass-through */
    ph_log_fn on_log;              /* diagnostics sink; NULL = silent */
    void *user_data;               /* passed to before_send / on_log */

    int rate_limit_per_sec; /* token-bucket cap on capture/exception events per
                             * second (burst == this value); <= 0 = unlimited.
                             * Stops a runaway loop from flooding ingestion. */
    const char *const *property_denylist; /* keys stripped from every event
                                           * before send; NULL = none */
    int property_denylist_count;          /* number of keys in property_denylist */

    int crash_handler; /* install the in-process signal_crash handler (POSIX
                        * signals / Windows SEH): a fatal native crash is
                        * persisted and shipped as a $exception on the next
                        * launch. Default 0 = off (opt-in - it installs
                        * process-global handlers). Requires offline_path.
                        * Native only; wasm ignores it. */

    int max_batch_bytes;   /* cap on serialized bytes per POST: a batch that would
                            * exceed it is split into several POSTs. 0 = no byte
                            * cap (count only). ph_config_defaults sets 1 MiB.
                            * An unsplittable event over the cap is dropped.
                            * Guards against oversized-body 413s. Native only. */
    ph_stats_fn on_stats;  /* periodic health snapshot; NULL = off (see ph_stats_fn) */
    int stats_interval_ms; /* emit on_stats every ~N ms; 0 = disabled (default).
                            * Native only; wasm ignores it. */
} ph_config;

/*
 * Fill `cfg` with defaults (all fields zeroed, then the documented default
 * numbers/policies applied). After this, set api_key, api_host, distinct_id,
 * and any overrides. Safe, recommended first step.
 */
void ph_config_defaults(ph_config *cfg);

/* --- Exceptions / error tracking --------------------------------
 *
 * "Exception" is PostHog's word for an Error Tracking event ($exception); it
 * has nothing to do with C++ throw/catch and works under -fno-exceptions.
 * Hand ph_capture_exception() a structured error and it ships as a $exception
 * event carrying the full $exception_list payload (type, value, mechanism, and
 * a raw stacktrace of your frames). Frames are optional.
 */
typedef struct ph_stackframe {
    const char *function; /* symbol name, if known */
    const char *filename; /* source file, if known */
    const char *module;   /* module/library name, if known */
    int lineno;           /* 0 if unknown */
    int in_app;           /* 1 if application code (vs. system/lib) */
} ph_stackframe;

typedef struct ph_exception {
    const char *type;    /* e.g. "NativeAssertion" */
    const char *message; /* human-readable value */
    int handled;         /* 1 = handled, 0 = uncaught */
    int synthetic;       /* 1 = synthesized (not a real throw) */
    const ph_stackframe *frames; /* optional, oldest-first or newest-first per your convention */
    int frame_count;
    const ph_props *extra; /* optional extra properties */
} ph_exception;

/* --- Lifecycle & capture ----------------------------------------- */

/*
 * Initialize the global SDK instance. Copies config, generates or adopts the
 * distinct id, starts the native sender thread. Returns PH_OK on success.
 * Calling when already initialized returns PH_ERR. If cfg->enabled is 0, the
 * SDK initializes into a no-op state and every call below returns quietly.
 * Over-cap string configuration is rejected with PH_ERR_BADARG rather than
 * silently truncated.
 */
ph_result ph_init(const ph_config *cfg);

/*
 * Capture an event. Fire-and-forget: copies the event name and accepted
 * property values into the SDK-owned ring and returns. No JSON, no network,
 * no malloc on the caller thread. `props` may be NULL for a bare event.
 * Oversized keys/values are truncated; over-capacity events drop properties
 * and bump an internal counter rather than borrowing caller memory.
 *
 * Returns the fate of this call's event: PH_OK if accepted into the ring,
 * PH_ERR_DISABLED if the SDK is off, PH_ERR_BADARG for a NULL/empty name, or
 * PH_ERR_TRUNCATED if an over-cap event name was accepted in truncated form, or
 * PH_ERR_RATE_LIMITED if the token bucket rejected it. Ring saturation still
 * returns PH_OK (your event is accepted; an older one is evicted) and surfaces
 * via ph_dropped_events()/on_stats, not here. Most callers ignore the return.
 */
ph_result ph_capture(const char *event, const ph_props *props);

/*
 * Identify the current user (sign-in). Emits $identify, applies set_props as
 * person properties ($set), and switches subsequent events to identified.
 * set_props may be NULL. The ID is capped to PH_DISTINCT_ID_CAP - 1 bytes.
 */
void ph_identify(const char *distinct_id, const ph_props *set_props);

/* Merge two ids into one person (emits $create_alias). IDs are capped to
 * PH_DISTINCT_ID_CAP - 1 bytes because this legacy void helper cannot report
 * PH_ERR_TRUNCATED. */
void ph_alias(const char *new_id, const char *old_id);

/* Logout: clear identity + super properties, roll a fresh anonymous id. */
void ph_reset(void);

/* Create/attach a group (emits $groupidentify; scopes later events via $groups).
 * Type/key are capped to PH_KEY_CAP - 1 bytes. */
void ph_group(const char *type, const char *key, const ph_props *set_props);

/* Register super properties: persisted, merged into every event. */
void ph_register(const ph_props *super_props);

/* Drop one super property by key. */
void ph_unregister(const char *key);

/* Report a structured error as a $exception event. This is the
 * app-reported ("posthog_exception") path - you caught something and want it
 * tracked. It is NOT a crash handler: for fatal native crashes, set
 * cfg.crash_handler (the signal_crash handler routes through here on the next
 * launch). */
void ph_capture_exception(const ph_exception *ex);

/* --- Feature flags ----------------------------------------------
 *
 * Remote evaluation (POST /flags/), read from a local cache. `preload_flags`
 * fetches during ph_init. Identity/group changes synchronously invalidate the
 * old evaluation context and schedule a background refresh; reads use their
 * supplied fallback (or return PH_ERR) until it completes. Call
 * ph_reload_feature_flags() after an identity/group change when the caller
 * requires fresh values before continuing.
 */
int ph_is_feature_enabled(const char *key, int fallback);

/*
 * Copy the resolved flag value (variant key, or "true"/"false") into out
 * (NUL-terminated, capped at cap). Returns PH_OK if the flag was found,
 * PH_ERR otherwise. out may be NULL to just test presence.
 */
ph_result ph_get_feature_flag(const char *key, char *out, int cap);

/* Copy the flag's JSON payload into out. Same return contract as above. */
ph_result ph_get_feature_flag_payload(const char *key, char *out, int cap);

/* Re-evaluate feature flags now. On native this is one blocking /flags/ round
 * trip; the SDK also refreshes flags during ph_init (preload_flags) and after
 * ph_identify. On wasm it asks posthog-js to reload. */
void ph_reload_feature_flags(void);

/* --- Draining --------------------------------------------------------- */

/*
 * Total events dropped so far: ring overflow (drop-oldest) plus rate-limit
 * rejections. A monotonic counter for health dashboards / diagnostics.
 */
uint64_t ph_dropped_events(void);

/*
 * Block until the queue drains or timeout_ms elapses (0 = don't wait, -1 =
 * wait indefinitely). Use before a short-lived process exits.
 */
void ph_flush(int timeout_ms);

/* Flush, stop the sender thread, and free all SDK resources. */
void ph_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* POSTHOG_H */
