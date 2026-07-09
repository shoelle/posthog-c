/*
 * ph_str.h - a minimal growable byte buffer.
 *
 * Used off the hot path (on the sender thread) to assemble serialized batch
 * bodies. On allocation failure it latches `oom` and every further append is a
 * no-op, so serialization degrades to a short/empty body instead of crashing.
 */
#ifndef PH_STR_H
#define PH_STR_H

#include <stddef.h>

typedef struct ph_strbuf {
    char *data;
    size_t len;
    size_t cap;
    int oom;
} ph_strbuf;

void ph_strbuf_init(ph_strbuf *b);
void ph_strbuf_free(ph_strbuf *b);
void ph_strbuf_reserve(ph_strbuf *b, size_t additional);
void ph_strbuf_append(ph_strbuf *b, const char *s, size_t n);
void ph_strbuf_append_cstr(ph_strbuf *b, const char *s);
void ph_strbuf_append_char(ph_strbuf *b, char c);

#endif /* PH_STR_H */
