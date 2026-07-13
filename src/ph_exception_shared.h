/*
 * ph_exception_shared.h - backend-neutral shaping for structured exceptions.
 *
 * Native and WASM both expose ph_capture_exception(). Keep the scalar
 * properties and nested $exception_list construction here so both backends
 * preserve the same caller-supplied type/value/mechanism/stack-frame shape.
 * Backend-owned privacy hooks still run in their respective implementation
 * before ph__exception_build_list() is called.
 */
#ifndef PH_EXCEPTION_SHARED_H
#define PH_EXCEPTION_SHARED_H

#include "posthog.h"
#include "ph_json.h"
#include "ph_str.h"
#include "ph_util.h"

#include <stddef.h>

#ifndef PH_MAX_EXCEPTION_FRAMES
#define PH_MAX_EXCEPTION_FRAMES 32
#endif
#ifndef PH_EXCEPTION_FIELD_CAP
#define PH_EXCEPTION_FIELD_CAP 96
#endif

static void ph__exception_prepare_base(const ph_exception *ex, ph_props *p) {
    int i;
    ph_props_init(p);
    ph_props_set_str(p, "$exception_level", ex->handled ? "warning" : "error");
    ph_props_set_str(p, "$exception_type", ex->type ? ex->type : "Error");
    ph_props_set_str(p, "$exception_message", ex->message ? ex->message : "");
    if (ex->extra) {
        for (i = 0; i < ex->extra->count; i++)
            ph_copy_prop_value(p, &ex->extra->items[i]);
    }
}

/* Extract the privacy-reviewed type/value and remove their temporary scalar
 * keys. The nested $exception_list becomes their one wire representation. */
static void ph__exception_take_type_message(ph_props *p,
                                            char *type, size_t type_cap,
                                            char *message, size_t message_cap) {
    const char *v;
    v = ph_props_find_last_str(p, "$exception_type");
    ph_copy_capped(type, type_cap, v ? v : "Error");
    v = ph_props_find_last_str(p, "$exception_message");
    ph_copy_capped(message, message_cap, v ? v : "");
    ph_props_remove_key(p, "$exception_type");
    ph_props_remove_key(p, "$exception_message");
}

/* Emit s as a JSON string literal, truncated to the shared exception-field
 * cap. This bounds both backend payloads independently of caller storage. */
static void ph__exception_json_cstr(ph_strbuf *out, const char *s) {
    size_t n = 0;
    if (!s) s = "";
    while (s[n] && n + 1 < PH_EXCEPTION_FIELD_CAP) n++;
    ph_json_str(out, s, n);
}

/* Build one structured PostHog exception entry. frame_budget == 0 means the
 * frame-count/field caps alone bound output; native supplies its fixed event
 * blob budget so an oversized list truncates before packing. */
static void ph__exception_build_list(ph_strbuf *out, const ph_exception *ex,
                                     const char *type, const char *message,
                                     int omit_function, int omit_filename,
                                     int omit_module, int omit_frames,
                                     size_t frame_budget) {
    int i;
    int frame_count = ex->frame_count;
    if (frame_count < 0) frame_count = 0;
    if (frame_count > PH_MAX_EXCEPTION_FRAMES)
        frame_count = PH_MAX_EXCEPTION_FRAMES;

    ph_strbuf_append_cstr(out, "[{\"type\":");
    ph__exception_json_cstr(out, type ? type : "Error");
    ph_strbuf_append_cstr(out, ",\"value\":");
    ph__exception_json_cstr(out, message ? message : "");
    ph_strbuf_append_cstr(out, ",\"mechanism\":{\"handled\":");
    ph_json_bool(out, ex->handled);
    ph_strbuf_append_cstr(out, ",\"synthetic\":");
    ph_json_bool(out, ex->synthetic);
    ph_strbuf_append_cstr(out,
                          "},\"stacktrace\":{\"type\":\"raw\",\"frames\":[");
    for (i = 0; !omit_frames && ex->frames && i < frame_count; i++) {
        const ph_stackframe *f = &ex->frames[i];
        if (frame_budget > 0 && out->len > frame_budget) break;
        if (i > 0) ph_strbuf_append_char(out, ',');
        ph_strbuf_append_cstr(
            out, "{\"platform\":\"custom\",\"lang\":\"cpp\",\"in_app\":");
        ph_json_bool(out, f->in_app);
        if (!omit_function && f->function) {
            ph_strbuf_append_cstr(out, ",\"function\":");
            ph__exception_json_cstr(out, f->function);
        }
        if (!omit_filename && f->filename) {
            ph_strbuf_append_cstr(out, ",\"filename\":");
            ph__exception_json_cstr(out, f->filename);
        }
        if (!omit_module && f->module) {
            ph_strbuf_append_cstr(out, ",\"module\":");
            ph__exception_json_cstr(out, f->module);
        }
        if (f->lineno > 0) {
            ph_strbuf_append_cstr(out, ",\"lineno\":");
            ph_json_int(out, f->lineno);
        }
        ph_strbuf_append_cstr(out, ",\"resolved\":true}");
    }
    ph_strbuf_append_cstr(out, "]}}]");
}

#endif /* PH_EXCEPTION_SHARED_H */
