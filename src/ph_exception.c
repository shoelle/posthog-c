/*
 * ph_exception.c - the posthog_exception path: turn a caller's ph_exception
 * into a $exception event (type / value / mechanism + a bounded raw stacktrace).
 * The app-reported ph_capture_exception and the signal_crash replay both funnel
 * through here. Off the capture hot path (exceptions are rare), so the caller's
 * transient frame pointers are copied out before the event is enqueued.
 */
#include "ph_internal.h"
#include "ph_exception_shared.h"

/* denylist_has / apply_denylist bind this backend's global denylist to the
 * shared ph_util implementation, used here to scrub exception fields. */
static int denylist_has(const char *key) {
    return ph_denylist_has(g_ph.denylist, g_ph.denylist_count, key);
}

static void apply_denylist(ph_props *p) {
    ph_apply_denylist(p, g_ph.denylist, g_ph.denylist_count);
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
    ph__exception_prepare_base(ex, p);

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

    ph__exception_take_type_message(p, type, type_cap, message, message_cap);

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
    ph__exception_build_list(
        &list, ex, type, message, omit_function, omit_filename, omit_module,
        omit_frames, (size_t)PH_EVENT_DATA_CAP - PH_EXCEPTION_BLOB_RESERVE);
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
