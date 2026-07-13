/*
 * ph_serialize.c - event records -> the PostHog /batch/ envelope.
 *
 * This is the one place event JSON is produced, and it is deliberately pure:
 * no threads, no clocks beyond the per-event reconstruction, no network. That
 * makes it directly unit-testable and is what native/wasm parity is asserted
 * against - both backends must produce the same event shape.
 *
 * Envelope (confirmed against the capture API):
 *   {"api_key":"...","historical_migration":false,"batch":[ <event>, ... ]}
 * Each <event>:
 *   {"event":"...","timestamp":"ISO-8601","uuid":"...","properties":{ ... }}
 * Note distinct_id lives *inside* properties for batch items.
 */
#include "ph_internal.h"
#include "ph_json.h"
#include "ph_str.h"
#include "ph_time.h"
#include "ph_util.h"

#include <string.h>

/* --- Compile-time platform tags (auto-properties) ----------------- */

static const char *platform_os(void) {
#if defined(__EMSCRIPTEN__)
    return "Web";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "Mac OS X";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return "BSD";
#else
    return "Unknown";
#endif
}

static const char *platform_arch(void) {
#if defined(__wasm__) || defined(__EMSCRIPTEN__)
    return "wasm32";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

static const char *backend_tag(void) {
#if defined(__EMSCRIPTEN__)
    return "wasm";
#else
    return "native";
#endif
}

/* The packed-blob reader (ph_blob_next) lives in ph_props.c so the binary
 * format has exactly one parser, shared with the sender's scrub path. */

static void emit_scalar_value(ph_strbuf *out, unsigned char type,
                              const char *val, size_t vlen) {
    switch (type) {
        case PH_T_STR:
            ph_json_str(out, val, vlen);
            break;
        case PH_T_DOUBLE: {
            double d = 0;
            if (vlen == sizeof(double)) memcpy(&d, val, sizeof(double));
            ph_json_double(out, d);
            break;
        }
        case PH_T_INT: {
            int64_t i = 0;
            if (vlen == sizeof(int64_t)) memcpy(&i, val, sizeof(int64_t));
            ph_json_int(out, i);
            break;
        }
        case PH_T_BOOL:
            ph_json_bool(out, vlen >= 1 ? val[0] : 0);
            break;
        default:
            ph_strbuf_append(out, "null", 4);
            break;
    }
}

static void comma(ph_strbuf *out, int *first) {
    if (*first) *first = 0;
    else ph_strbuf_append_char(out, ',');
}

static void emit_kv_cstr(ph_strbuf *out, int *first, const char *key,
                         const char *val) {
    comma(out, first);
    ph_json_cstr(out, key);
    ph_strbuf_append_char(out, ':');
    ph_json_cstr(out, val);
}

static int key_is(const char *key, size_t klen, const char *lit) {
    return klen == strlen(lit) && memcmp(key, lit, klen) == 0;
}

static int key_is_sdk_owned_top_level(const char *key, size_t klen) {
    return key_is(key, klen, "distinct_id") ||
           key_is(key, klen, "$lib") ||
           key_is(key, klen, "$lib_version") ||
           key_is(key, klen, "$lib_backend") ||
           key_is(key, klen, "$process_person_profile");
}

/* Emit every scalar (non-group) entry as a top-level "key":value pair. */
static void emit_scalar_entries(ph_strbuf *out, int *first, const char *blob,
                                size_t blob_len, int skip_geoip_disable,
                                int skip_sdk_owned) {
    const char *cur = blob, *end = blob + blob_len;
    unsigned char type;
    const char *key, *val;
    size_t klen, vlen;
    while (ph_blob_next(&cur, end, &type, &key, &klen, &val, &vlen)) {
        if (type == PH_PK_GROUP) continue;
        if (skip_geoip_disable && key_is(key, klen, "$geoip_disable")) continue;
        if (skip_sdk_owned && key_is_sdk_owned_top_level(key, klen)) continue;
        comma(out, first);
        ph_json_str(out, key, klen);
        ph_strbuf_append_char(out, ':');
        if (type == PH_PK_RAWJSON)
            ph_strbuf_append(out, val, vlen); /* pre-serialized JSON, emit verbatim */
        else
            emit_scalar_value(out, type, val, vlen);
    }
}

static int blob_has_scalar_key(const char *blob, size_t blob_len,
                               const char *wanted) {
    const char *cur = blob, *end = blob + blob_len;
    unsigned char type;
    const char *key, *val;
    size_t klen, vlen;
    while (ph_blob_next(&cur, end, &type, &key, &klen, &val, &vlen)) {
        (void)val;
        (void)vlen;
        if (type <= PH_T_BOOL && key_is(key, klen, wanted)) return 1;
    }
    return 0;
}

/* Emit "$groups":{type:key,...} if the blob carries any group entries. */
static void emit_groups(ph_strbuf *out, int *first, const char *blob,
                        size_t blob_len) {
    const char *cur = blob, *end = blob + blob_len;
    unsigned char type;
    const char *key, *val;
    size_t klen, vlen;
    int have = 0, inner_first = 1;
    while (ph_blob_next(&cur, end, &type, &key, &klen, &val, &vlen)) {
        if (type != PH_PK_GROUP) continue;
        if (!have) {
            comma(out, first);
            ph_strbuf_append_cstr(out, "\"$groups\":{");
            have = 1;
        }
        comma(out, &inner_first);
        ph_json_str(out, key, klen);
        ph_strbuf_append_char(out, ':');
        ph_json_str(out, val, vlen);
    }
    if (have) ph_strbuf_append_char(out, '}');
}

/* The $groupidentify shape: $group_type/$group_key top-level, everything else
 * bundled under $group_set. */
static void emit_group_identify(ph_strbuf *out, int *first, const char *blob,
                                size_t blob_len) {
    const char *cur, *end = blob + blob_len;
    unsigned char type;
    const char *key, *val;
    size_t klen, vlen;
    int set_first = 1, have_set = 0;

    cur = blob;
    while (ph_blob_next(&cur, end, &type, &key, &klen, &val, &vlen)) {
        if (key_is(key, klen, "$group_type") || key_is(key, klen, "$group_key")) {
            comma(out, first);
            ph_json_str(out, key, klen);
            ph_strbuf_append_char(out, ':');
            emit_scalar_value(out, type, val, vlen);
        }
    }
    cur = blob;
    while (ph_blob_next(&cur, end, &type, &key, &klen, &val, &vlen)) {
        if (key_is(key, klen, "$group_type") || key_is(key, klen, "$group_key"))
            continue;
        if (!have_set) {
            comma(out, first);
            ph_strbuf_append_cstr(out, "\"$group_set\":{");
            have_set = 1;
        }
        comma(out, &set_first);
        ph_json_str(out, key, klen);
        ph_strbuf_append_char(out, ':');
        emit_scalar_value(out, type, val, vlen);
    }
    if (have_set) ph_strbuf_append_char(out, '}');
}

static void serialize_event(const ph_ctx *ctx, const ph_event *e,
                            ph_strbuf *out) {
    const char *name = e->data;
    const char *did = e->data + e->name_len;
    const char *blob = e->data + e->name_len + e->did_len;
    const char *eff_did;
    size_t eff_did_len;
    uint64_t delta, wall;
    char iso[40];
    char uuid[37];
    int first = 1;
    int props_are_top_level = e->kind != PH_EV_IDENTIFY && e->kind != PH_EV_GROUP;

    if (e->flags & PH_EVF_HAS_DID) {
        eff_did = did;
        eff_did_len = e->did_len;
    } else {
        eff_did = ctx->distinct_id;
        eff_did_len = strlen(ctx->distinct_id);
    }

    /* Reconstruct wall-clock time from the monotonic tick against the epoch
     * snapshot captured with the event. Sender-side clock corrections update
     * the global epoch only for later events, not already queued records. */
    delta = (e->mono_ns >= e->epoch_mono_ns) ? (e->mono_ns - e->epoch_mono_ns) : 0;
    wall = e->epoch_wall_ns + delta;
    ph_format_iso8601(wall, iso, sizeof(iso));
    ph_uuid_v7(wall / 1000000ull, ctx->uuid_salt, e->seq, uuid);

    ph_strbuf_append_cstr(out, "{\"event\":");
    ph_json_str(out, name, e->name_len);
    ph_strbuf_append_cstr(out, ",\"timestamp\":");
    ph_json_cstr(out, iso);
    ph_strbuf_append_cstr(out, ",\"uuid\":");
    ph_json_cstr(out, uuid);
    ph_strbuf_append_cstr(out, ",\"properties\":{");

    /* distinct_id lives inside properties for batch items. */
    comma(out, &first);
    ph_strbuf_append_cstr(out, "\"distinct_id\":");
    ph_json_str(out, eff_did, eff_did_len);

    /* Required SDK properties are always stamped. Optional platform/release
     * properties honor the privacy denylist, and an explicit event property
     * suppresses its automatic counterpart rather than relying on duplicate
     * JSON-key ordering for precedence. */
    emit_kv_cstr(out, &first, "$lib", "posthog-c");
    emit_kv_cstr(out, &first, "$lib_version", PH_VERSION_STRING);
    emit_kv_cstr(out, &first, "$lib_backend", backend_tag());
    if (!ph_denylist_has(ctx->denylist, ctx->denylist_count, "$os") &&
        (!props_are_top_level || !blob_has_scalar_key(blob, e->blob_len, "$os")))
        emit_kv_cstr(out, &first, "$os", platform_os());
    if (!ph_denylist_has(ctx->denylist, ctx->denylist_count, "arch") &&
        (!props_are_top_level || !blob_has_scalar_key(blob, e->blob_len, "arch")))
        emit_kv_cstr(out, &first, "arch", platform_arch());
    if (ctx->release[0] &&
        !ph_denylist_has(ctx->denylist, ctx->denylist_count, "release") &&
        (!props_are_top_level || !blob_has_scalar_key(blob, e->blob_len, "release")))
        emit_kv_cstr(out, &first, "release", ctx->release);

    if (e->flags & PH_EVF_NO_PROFILE) {
        comma(out, &first);
        ph_strbuf_append_cstr(out, "\"$process_person_profile\":false");
    }

    switch (e->kind) {
        case PH_EV_IDENTIFY: {
            /* set_props ride under $set as person properties. */
            comma(out, &first);
            ph_strbuf_append_cstr(out, "\"$set\":{");
            {
                int sf = 1;
                emit_scalar_entries(out, &sf, blob, e->blob_len, 0, 0);
            }
            ph_strbuf_append_char(out, '}');
            break;
        }
        case PH_EV_GROUP:
            emit_group_identify(out, &first, blob, e->blob_len);
            break;
        default:
            /* CAPTURE / EXCEPTION / ALIAS: user + super props top-level, plus
             * any group-scoping entries under $groups. */
            emit_scalar_entries(out, &first, blob, e->blob_len,
                                ctx->disable_geoip, 1);
            emit_groups(out, &first, blob, e->blob_len);
            break;
    }

    /* A configured privacy opt-out is a final wire invariant: caller/super
     * properties and before_send cannot replace or remove it, and the config
     * denylist cannot accidentally disable it. */
    if (ctx->disable_geoip) {
        comma(out, &first);
        ph_strbuf_append_cstr(out, "\"$geoip_disable\":true");
    }

    ph_strbuf_append_cstr(out, "}}");
}

void ph_serialize_batch(const ph_ctx *ctx, const ph_event *events, int n,
                        ph_strbuf *out) {
    int i;
    ph_strbuf_append_cstr(out, "{\"api_key\":");
    ph_json_cstr(out, ctx->api_key);
    ph_strbuf_append_cstr(out, ",\"historical_migration\":false,\"batch\":[");
    for (i = 0; i < n; i++) {
        if (i > 0) ph_strbuf_append_char(out, ',');
        serialize_event(ctx, &events[i], out);
    }
    ph_strbuf_append_cstr(out, "]}");
}

/* Serialize a ph_props to a JSON object `{"k":v,...}` with the same encoder the
 * native batch path uses. The WASM shim calls this to build the properties it
 * hands to window.posthog.capture, so a given ph_props yields byte-identical
 * property JSON on both backends - the core of native/wasm parity. */
void ph_serialize_props_object(const ph_props *p, ph_strbuf *out) {
    int i, first = 1;
    ph_strbuf_append_char(out, '{');
    for (i = 0; p && i < p->count; i++) {
        const ph_prop *it = &p->items[i];
        comma(out, &first);
        ph_json_cstr(out, it->key);
        ph_strbuf_append_char(out, ':');
        switch (it->type) {
            case PH_T_STR: ph_json_cstr(out, it->val.str); break;
            case PH_T_DOUBLE: ph_json_double(out, it->val.dbl); break;
            case PH_T_INT: ph_json_int(out, it->val.i64); break;
            case PH_T_BOOL: ph_json_bool(out, it->val.boolean); break;
            default: ph_strbuf_append_cstr(out, "null"); break;
        }
    }
    ph_strbuf_append_char(out, '}');
}
