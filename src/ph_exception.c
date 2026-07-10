/*
 * ph_exception.c - the posthog_exception path: turn a caller's ph_exception
 * into a $exception event (type / value / mechanism + a bounded raw stacktrace).
 * The app-reported ph_capture_exception and the signal_crash replay both funnel
 * through here. Off the capture hot path (exceptions are rare), so the caller's
 * transient frame pointers are copied out before the event is enqueued.
 */
#include "ph_internal.h"
#include "ph_json.h"
#include "ph_str.h"
#include "ph_util.h"

/* denylist_has / apply_denylist bind this backend's global denylist to the
 * shared ph_util implementation, used here to scrub exception fields. */
static int denylist_has(const char *key) {
    return ph_denylist_has(g_ph.denylist, g_ph.denylist_count, key);
}

static void apply_denylist(ph_props *p) {
    ph_apply_denylist(p, g_ph.denylist, g_ph.denylist_count);
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
        for (i = 0; i < ex->extra->count; i++) ph_copy_prop_value(p, &ex->extra->items[i]);
    }

    apply_denylist(p);
    if (denylist_has("type")) ph_props_remove_key(p, "$exception_type");
    if (denylist_has("message")) ph_props_remove_key(p, "$exception_message");

    if (g_ph.before_send) {
        int keep;
        ph__in_callback++;
        keep = g_ph.before_send("$exception", p, g_ph.user_data);
        ph__in_callback--;
        if (!keep) return 0;
    }

    v = ph_props_find_last_str(p, "$exception_type");
    ph_copy_capped(type, type_cap, v ? v : "Error");
    v = ph_props_find_last_str(p, "$exception_message");
    ph_copy_capped(message, message_cap, v ? v : "");

    ph_props_remove_key(p, "$exception_type");
    ph_props_remove_key(p, "$exception_message");

    *omit_function = denylist_has("function") || denylist_has("$exception_frame_function");
    *omit_filename = denylist_has("filename") || denylist_has("$exception_frame_filename");
    *omit_module = denylist_has("module") || denylist_has("$exception_frame_module");
    *omit_frames = denylist_has("frames") || denylist_has("stacktrace") ||
                   denylist_has("$exception_frames");
    return 1;
}

ph_result ph__capture_exception_flags(const ph_exception *ex,
                                      unsigned char base_flags) {
    ph_props p;
    ph_strbuf list;
    char extra[PH_EVENT_DATA_CAP];
    size_t extra_len = 0;
    char type[PH_EXCEPTION_FIELD_CAP];
    char message[PH_EXCEPTION_FIELD_CAP];
    int omit_function, omit_filename, omit_module, omit_frames;

    ph_result result;
    if (!g_ph.enabled) return PH_ERR_DISABLED;
    if (!ex) return PH_ERR_BADARG;

    /* The structured exception payload has to be copied before caller-owned
     * pointers go stale. Run the privacy hook here for exceptions so type/message
     * can be redacted before the raw $exception_list is built. */
    if (!prepare_exception_props(ex, &p, type, sizeof(type), message, sizeof(message),
                                 &omit_function, &omit_filename, &omit_module,
                                 &omit_frames))
        return PH_ERR;

    /* The nested $exception_list rides as a rawjson entry (the flat packer can't
     * express nested arrays/objects); the serializer emits it verbatim. */
    ph_strbuf_init(&list);
    build_exception_list(&list, ex, type, message, omit_function, omit_filename,
                         omit_module, omit_frames);
    if (list.data && !list.oom)
        extra_len = ph_pack_str_entry(extra, sizeof(extra), (unsigned char)PH_PK_RAWJSON,
                                      "$exception_list", list.data);

    result = ph__submit_event(PH_EV_EXCEPTION,
                              (unsigned char)(PH_EVF_SCRUBBED | base_flags),
                              "$exception", NULL, &p, -1, 1, extra, extra_len);
    ph_strbuf_free(&list);
    return result;
}

void ph_capture_exception(const ph_exception *ex) {
    (void)ph__capture_exception_flags(ex, 0);
}
