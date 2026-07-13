/*
 * The serializer is the parity-critical piece: it defines the exact wire shape
 * both the native and (future) wasm backends must produce. These tests build a
 * context + event records by hand and assert the JSON substrings.
 */
#include "ph_internal.h"
#include "ph_str.h"
#include "ph_time.h"
#include "test_util.h"

#include <string.h>

static void setup_ctx(ph_ctx *c) {
    memset(c, 0, sizeof(*c));
    strcpy(c->api_key, "phc_test123");
    strcpy(c->distinct_id, "ctx-default");
    c->epoch_wall_ns = 1751803200000000000ull; /* ~2025-07-06T12:00:00Z */
    c->epoch_mono_ns = 1000;
    c->uuid_salt = 424242;
    c->person_profiles = PH_IDENTIFIED_ONLY;
}

static void build_event(ph_event *e, int kind, unsigned char flags,
                        const char *name, const char *did,
                        const ph_props *props) {
    size_t off;
    memset(e, 0, sizeof(*e));
    e->kind = (uint8_t)kind;
    e->flags = (uint8_t)(flags | PH_EVF_HAS_DID);
    e->mono_ns = 1000;
    e->epoch_wall_ns = 1751803200000000000ull;
    e->epoch_mono_ns = 1000;
    e->seq = 7;
    e->name_len = (uint16_t)strlen(name);
    e->did_len = (uint16_t)strlen(did);
    memcpy(e->data, name, e->name_len);
    memcpy(e->data + e->name_len, did, e->did_len);
    off = (size_t)e->name_len + e->did_len;
    e->blob_len = (uint16_t)ph_pack_props(props, e->data + off, PH_EVENT_DATA_CAP - off);
}

static int count_occurrences(const char *hay, const char *needle) {
    int n = 0;
    size_t len = strlen(needle);
    while (hay && (hay = strstr(hay, needle)) != NULL) {
        n++;
        hay += len;
    }
    return n;
}

static void set_sdk_owned_attempts(ph_props *p) {
    ph_props_set_str(p, "distinct_id", "shadow-distinct");
    ph_props_set_str(p, "$lib", "shadow-lib");
    ph_props_set_str(p, "$lib_version", "shadow-version");
    ph_props_set_str(p, "$lib_backend", "shadow-backend");
    ph_props_set_bool(p, "$process_person_profile", 1);
}

void suite_serialize(void) {
    ph_ctx c;
    ph_event e;
    ph_props p;
    ph_strbuf out;
    setup_ctx(&c);

    /* --- sender-side clock correction ignores jitter but follows real jumps --- */
    CHECK(ph_correct_wall_epoch(1000, 100, 1150, 200, 50) == 1000);
    CHECK(ph_correct_wall_epoch(1000, 100, 2000, 200, 50) == 1900);
    CHECK(ph_correct_wall_epoch(1000, 100, 500, 200, 50) == 400);
    CHECK(ph_correct_wall_epoch(1000, 100, 9999, 99, 0) == 1000);

    /* --- each event carries its own clock epoch snapshot --- */
    {
        ph_event evs[2];
        char old_iso[40], new_iso[40];
        ph_props_init(&p);
        build_event(&evs[0], PH_EV_CAPTURE, 0, "old_epoch", "u", &p);
        build_event(&evs[1], PH_EV_CAPTURE, 0, "new_epoch", "u", &p);
        evs[1].epoch_wall_ns = c.epoch_wall_ns + 10000000000ull;
        ph_format_iso8601(evs[0].epoch_wall_ns, old_iso, sizeof(old_iso));
        ph_format_iso8601(evs[1].epoch_wall_ns, new_iso, sizeof(new_iso));
        ph_strbuf_init(&out);
        ph_serialize_batch(&c, evs, 2, &out);
        CHECK_CONTAINS(out.data, old_iso);
        CHECK_CONTAINS(out.data, new_iso);
        ph_strbuf_free(&out);
    }

    /* --- basic capture, mixed prop types, anonymous --- */
    ph_props_init(&p);
    ph_props_set_str(&p, "weapon", "sword");
    ph_props_set_int(&p, "level", 3);
    ph_props_set_double(&p, "score", 1.5);
    ph_props_set_bool(&p, "alive", 1);
    build_event(&e, PH_EV_CAPTURE, PH_EVF_NO_PROFILE, "level_started", "user-1", &p);
    ph_strbuf_init(&out);
    ph_serialize_batch(&c, &e, 1, &out);
    CHECK_CONTAINS(out.data, "\"api_key\":\"phc_test123\"");
    CHECK_CONTAINS(out.data, "\"historical_migration\":false");
    CHECK_CONTAINS(out.data, "\"batch\":[");
    CHECK_CONTAINS(out.data, "\"event\":\"level_started\"");
    CHECK_CONTAINS(out.data, "\"distinct_id\":\"user-1\""); /* inside properties */
    CHECK_CONTAINS(out.data, "\"$lib\":\"posthog-c\"");
    CHECK_CONTAINS(out.data, "\"$lib_version\":\"0.1.0\"");
    CHECK_CONTAINS(out.data, "\"$lib_backend\":\"native\"");
    CHECK_CONTAINS(out.data, "\"$os\":");
    CHECK_CONTAINS(out.data, "\"weapon\":\"sword\"");
    CHECK_CONTAINS(out.data, "\"level\":3");
    CHECK_CONTAINS(out.data, "\"score\":1.5");
    CHECK_CONTAINS(out.data, "\"alive\":true");
    CHECK_CONTAINS(out.data, "\"$process_person_profile\":false");
    CHECK_CONTAINS(out.data, "\"timestamp\":\"2");
    CHECK_CONTAINS(out.data, "\"uuid\":\"");
    CHECK_NOT_CONTAINS(out.data, "$geoip_disable");
    ph_strbuf_free(&out);

    /* --- SDK-owned top-level fields cannot be duplicated or overridden --- */
    {
        ph_event evs[3];
        const int kinds[3] = {PH_EV_CAPTURE, PH_EV_EXCEPTION, PH_EV_ALIAS};
        const char *names[3] = {"owned_capture", "$exception", "$create_alias"};
        int i;
        setup_ctx(&c);
        ph_props_init(&p);
        set_sdk_owned_attempts(&p);
        for (i = 0; i < 3; i++)
            build_event(&evs[i], kinds[i], PH_EVF_NO_PROFILE, names[i],
                        "wire-distinct", &p);
        ph_strbuf_init(&out);
        ph_serialize_batch(&c, evs, 3, &out);
        CHECK(count_occurrences(out.data, "\"distinct_id\":") == 3);
        CHECK(count_occurrences(out.data, "\"distinct_id\":\"wire-distinct\"") == 3);
        CHECK(count_occurrences(out.data, "\"$lib\":") == 3);
        CHECK(count_occurrences(out.data, "\"$lib\":\"posthog-c\"") == 3);
        CHECK(count_occurrences(out.data, "\"$lib_version\":") == 3);
        CHECK(count_occurrences(out.data, "\"$lib_version\":\"0.1.0\"") == 3);
        CHECK(count_occurrences(out.data, "\"$lib_backend\":") == 3);
        CHECK(count_occurrences(out.data, "\"$lib_backend\":\"native\"") == 3);
        CHECK(count_occurrences(out.data, "\"$process_person_profile\":") == 3);
        CHECK(count_occurrences(out.data,
                                "\"$process_person_profile\":false") == 3);
        CHECK_NOT_CONTAINS(out.data, "shadow-distinct");
        CHECK_NOT_CONTAINS(out.data, "shadow-lib");
        CHECK_NOT_CONTAINS(out.data, "shadow-version");
        CHECK_NOT_CONTAINS(out.data, "shadow-backend");
        CHECK_NOT_CONTAINS(out.data, "\"$process_person_profile\":true");
        ph_strbuf_free(&out);
    }

    /* --- identify keeps same-named person properties inside $set --- */
    {
        setup_ctx(&c);
        ph_props_init(&p);
        set_sdk_owned_attempts(&p);
        build_event(&e, PH_EV_IDENTIFY, 0, "$identify", "identified-user", &p);
        ph_strbuf_init(&out);
        ph_serialize_batch(&c, &e, 1, &out);
        CHECK_CONTAINS(out.data, "\"distinct_id\":\"identified-user\"");
        CHECK_CONTAINS(out.data,
                       "\"$set\":{\"distinct_id\":\"shadow-distinct\"");
        CHECK_CONTAINS(out.data, "\"$lib\":\"shadow-lib\"");
        CHECK_CONTAINS(out.data, "\"$lib_version\":\"shadow-version\"");
        CHECK_CONTAINS(out.data, "\"$lib_backend\":\"shadow-backend\"");
        CHECK_CONTAINS(out.data, "\"$process_person_profile\":true");
        CHECK(count_occurrences(out.data, "\"distinct_id\":") == 2);
        CHECK(count_occurrences(out.data, "\"$lib\":") == 2);
        CHECK(count_occurrences(out.data, "\"$lib_version\":") == 2);
        CHECK(count_occurrences(out.data, "\"$lib_backend\":") == 2);
        CHECK(count_occurrences(out.data, "\"$process_person_profile\":") == 1);
        ph_strbuf_free(&out);
    }

    /* --- GeoIP opt-out is final on every native event kind --- */
    {
        ph_event evs[5];
        const int kinds[5] = {PH_EV_CAPTURE, PH_EV_IDENTIFY, PH_EV_ALIAS,
                              PH_EV_GROUP, PH_EV_EXCEPTION};
        const char *names[5] = {"capture", "$identify", "$create_alias",
                                "$groupidentify", "$exception"};
        int i;
        setup_ctx(&c);
        c.disable_geoip = 1;
        ph_props_init(&p);
        for (i = 0; i < 5; i++)
            build_event(&evs[i], kinds[i], 0, names[i], "geo-user", &p);
        ph_strbuf_init(&out);
        ph_serialize_batch(&c, evs, 5, &out);
        CHECK(count_occurrences(out.data, "\"$geoip_disable\":true") == 5);
        ph_strbuf_free(&out);
    }

    /* --- configured GeoIP policy replaces caller attempts and ignores denylist --- */
    {
        setup_ctx(&c);
        c.disable_geoip = 1;
        strcpy(c.denylist[0], "$geoip_disable");
        c.denylist_count = 1;
        ph_props_init(&p);
        ph_props_set_bool(&p, "$geoip_disable", 0);
        build_event(&e, PH_EV_CAPTURE, 0, "geo_override", "geo-user", &p);
        ph_strbuf_init(&out);
        ph_serialize_batch(&c, &e, 1, &out);
        CHECK_NOT_CONTAINS(out.data, "\"$geoip_disable\":false");
        CHECK(count_occurrences(out.data, "\"$geoip_disable\":true") == 1);
        ph_strbuf_free(&out);
    }

    /* --- optional auto-properties honor denylist --- */
    {
        setup_ctx(&c);
        strcpy(c.release, "configured-release");
        strcpy(c.denylist[0], "$os");
        strcpy(c.denylist[1], "arch");
        strcpy(c.denylist[2], "release");
        c.denylist_count = 3;
        ph_props_init(&p);
        build_event(&e, PH_EV_CAPTURE, 0, "denylisted_auto", "user", &p);
        ph_strbuf_init(&out);
        ph_serialize_batch(&c, &e, 1, &out);
        CHECK_NOT_CONTAINS(out.data, "\"$os\":");
        CHECK_NOT_CONTAINS(out.data, "\"arch\":");
        CHECK_NOT_CONTAINS(out.data, "\"release\":");
        ph_strbuf_free(&out);
    }

    /* --- explicit capture properties take precedence without duplicate keys --- */
    {
        setup_ctx(&c);
        strcpy(c.release, "configured-release");
        ph_props_init(&p);
        ph_props_set_str(&p, "$os", "CallerOS");
        ph_props_set_str(&p, "arch", "caller-arch");
        ph_props_set_str(&p, "release", "caller-release");
        build_event(&e, PH_EV_CAPTURE, 0, "explicit_auto", "user", &p);
        ph_strbuf_init(&out);
        ph_serialize_batch(&c, &e, 1, &out);
        CHECK_CONTAINS(out.data, "\"$os\":\"CallerOS\"");
        CHECK_CONTAINS(out.data, "\"arch\":\"caller-arch\"");
        CHECK_CONTAINS(out.data, "\"release\":\"caller-release\"");
        CHECK_NOT_CONTAINS(out.data, "configured-release");
        CHECK(count_occurrences(out.data, "\"$os\":") == 1);
        CHECK(count_occurrences(out.data, "\"arch\":") == 1);
        CHECK(count_occurrences(out.data, "\"release\":") == 1);
        ph_strbuf_free(&out);
    }

    setup_ctx(&c);

    /* --- identified capture omits the anonymous marker --- */
    ph_props_init(&p);
    build_event(&e, PH_EV_CAPTURE, 0, "ping", "user-2", &p);
    ph_strbuf_init(&out);
    ph_serialize_batch(&c, &e, 1, &out);
    CHECK_NOT_CONTAINS(out.data, "$process_person_profile");
    ph_strbuf_free(&out);

    /* --- identify wraps props under $set --- */
    ph_props_init(&p);
    ph_props_set_str(&p, "plan", "pro");
    build_event(&e, PH_EV_IDENTIFY, 0, "$identify", "acct-9", &p);
    ph_strbuf_init(&out);
    ph_serialize_batch(&c, &e, 1, &out);
    CHECK_CONTAINS(out.data, "\"event\":\"$identify\"");
    CHECK_CONTAINS(out.data, "\"$set\":{");
    CHECK_CONTAINS(out.data, "\"plan\":\"pro\"");
    CHECK_CONTAINS(out.data, "\"distinct_id\":\"acct-9\"");
    ph_strbuf_free(&out);

    /* --- $groups scoping via a group entry appended to the blob --- */
    {
        size_t off, bl;
        ph_props_init(&p);
        ph_props_set_int(&p, "round", 2);
        build_event(&e, PH_EV_CAPTURE, 0, "match_started", "user-3", &p);
        off = (size_t)e.name_len + e.did_len;
        bl = e.blob_len;
        bl += ph_pack_str_entry(e.data + off + bl, PH_EVENT_DATA_CAP - off - bl,
                                (unsigned char)PH_PK_GROUP, "game", "asteroids");
        e.blob_len = (uint16_t)bl;
        ph_strbuf_init(&out);
        ph_serialize_batch(&c, &e, 1, &out);
        CHECK_CONTAINS(out.data, "\"$groups\":{");
        CHECK_CONTAINS(out.data, "\"game\":\"asteroids\"");
        CHECK_CONTAINS(out.data, "\"round\":2");
        ph_strbuf_free(&out);
    }

    /* --- $groupidentify shape --- */
    ph_props_init(&p);
    ph_props_set_str(&p, "$group_type", "game");
    ph_props_set_str(&p, "$group_key", "asteroids");
    ph_props_set_str(&p, "name", "Asteroids");
    build_event(&e, PH_EV_GROUP, 0, "$groupidentify", "$game_asteroids", &p);
    ph_strbuf_init(&out);
    ph_serialize_batch(&c, &e, 1, &out);
    CHECK_CONTAINS(out.data, "\"event\":\"$groupidentify\"");
    CHECK_CONTAINS(out.data, "\"$group_type\":\"game\"");
    CHECK_CONTAINS(out.data, "\"$group_key\":\"asteroids\"");
    CHECK_CONTAINS(out.data, "\"$group_set\":{");
    CHECK_CONTAINS(out.data, "\"name\":\"Asteroids\"");
    ph_strbuf_free(&out);

    /* --- props-object serializer (shared with the wasm backend) --- */
    ph_props_init(&p);
    ph_props_set_str(&p, "weapon", "sword");
    ph_props_set_int(&p, "level", 3);
    ph_props_set_double(&p, "score", 1.5);
    ph_props_set_bool(&p, "alive", 1);
    ph_strbuf_init(&out);
    ph_serialize_props_object(&p, &out);
    /* Exact string the wasm shim hands to JSON.parse -> window.posthog.capture. */
    CHECK(strcmp(out.data,
                 "{\"weapon\":\"sword\",\"level\":3,\"score\":1.5,\"alive\":true}") == 0);
    ph_strbuf_free(&out);

    /* --- multi-event batch: two comma-separated objects --- */
    {
        ph_event evs[2];
        ph_props_init(&p); ph_props_set_int(&p, "a", 1);
        build_event(&evs[0], PH_EV_CAPTURE, 0, "e1", "u", &p);
        ph_props_init(&p); ph_props_set_int(&p, "b", 2);
        build_event(&evs[1], PH_EV_CAPTURE, 0, "e2", "u", &p);
        ph_strbuf_init(&out);
        ph_serialize_batch(&c, evs, 2, &out);
        CHECK_CONTAINS(out.data, "\"event\":\"e1\"");
        CHECK_CONTAINS(out.data, "\"event\":\"e2\"");
        CHECK_CONTAINS(out.data, "},{");
        ph_strbuf_free(&out);
    }
}
