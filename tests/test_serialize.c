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
    e->seq = 7;
    e->name_len = (uint16_t)strlen(name);
    e->did_len = (uint16_t)strlen(did);
    memcpy(e->data, name, e->name_len);
    memcpy(e->data + e->name_len, did, e->did_len);
    off = (size_t)e->name_len + e->did_len;
    e->blob_len = (uint16_t)ph_pack_props(props, e->data + off, PH_EVENT_DATA_CAP - off);
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
    CHECK_CONTAINS(out.data, "\"$lib_version\":\"0.7.0\"");
    CHECK_CONTAINS(out.data, "\"$lib_backend\":\"native\"");
    CHECK_CONTAINS(out.data, "\"$os\":");
    CHECK_CONTAINS(out.data, "\"weapon\":\"sword\"");
    CHECK_CONTAINS(out.data, "\"level\":3");
    CHECK_CONTAINS(out.data, "\"score\":1.5");
    CHECK_CONTAINS(out.data, "\"alive\":true");
    CHECK_CONTAINS(out.data, "\"$process_person_profile\":false");
    CHECK_CONTAINS(out.data, "\"timestamp\":\"2");
    CHECK_CONTAINS(out.data, "\"uuid\":\"");
    ph_strbuf_free(&out);

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
