#include "ph_str.h"

#include <stdlib.h>
#include <string.h>

void ph_strbuf_init(ph_strbuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = 0;
}

void ph_strbuf_free(ph_strbuf *b) {
    free(b->data);
    ph_strbuf_init(b);
}

void ph_strbuf_reserve(ph_strbuf *b, size_t additional) {
    size_t need;
    size_t newcap;
    char *p;

    if (b->oom) return;
    need = b->len + additional + 1; /* +1 for a trailing NUL */
    if (need <= b->cap) return;

    newcap = b->cap ? b->cap : 64;
    while (newcap < need) newcap *= 2;

    p = (char *)realloc(b->data, newcap);
    if (!p) {
        b->oom = 1;
        return;
    }
    b->data = p;
    b->cap = newcap;
}

void ph_strbuf_append(ph_strbuf *b, const char *s, size_t n) {
    if (b->oom || n == 0) return;
    ph_strbuf_reserve(b, n);
    if (b->oom) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void ph_strbuf_append_cstr(ph_strbuf *b, const char *s) {
    if (!s) return;
    ph_strbuf_append(b, s, strlen(s));
}

void ph_strbuf_append_char(ph_strbuf *b, char c) {
    ph_strbuf_append(b, &c, 1);
}
