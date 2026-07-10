/*
 * ph_util.c - shared ph_props helpers (see ph_util.h). Pure, no globals, so it
 * links into both the native library and the wasm module unchanged.
 */
#include "ph_util.h"

#include <string.h>

void ph_copy_capped(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (cap == 0) return;
    if (src)
        for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

void ph_copy_prop_value(ph_props *dst, const ph_prop *src) {
    switch (src->type) {
        case PH_T_STR: ph_props_set_str(dst, src->key, src->val.str); break;
        case PH_T_DOUBLE: ph_props_set_double(dst, src->key, src->val.dbl); break;
        case PH_T_INT: ph_props_set_int(dst, src->key, src->val.i64); break;
        case PH_T_BOOL: ph_props_set_bool(dst, src->key, src->val.boolean); break;
        default: break;
    }
}

static int props_has_key(const ph_props *p, const char *key) {
    int i;
    if (!p || !key) return 0;
    for (i = 0; i < p->count && i < PH_MAX_PROPS; i++)
        if (strcmp(p->items[i].key, key) == 0) return 1;
    return 0;
}

void ph_props_merge(ph_props *out, const ph_props *explicit_props,
                    const ph_props *super_props) {
    int i, n;
    ph_props_init(out);
    n = explicit_props && explicit_props->count > 0 ? explicit_props->count : 0;
    if (n > PH_MAX_PROPS) n = PH_MAX_PROPS;
    for (i = 0; i < n; i++) ph_copy_prop_value(out, &explicit_props->items[i]);
    n = super_props && super_props->count > 0 ? super_props->count : 0;
    if (n > PH_MAX_PROPS) n = PH_MAX_PROPS;
    for (i = 0; i < n && out->count < PH_MAX_PROPS; i++) {
        const ph_prop *it = &super_props->items[i];
        if (!props_has_key(explicit_props, it->key)) ph_copy_prop_value(out, it);
    }
}

void ph_props_remove_key(ph_props *p, const char *key) {
    int i, k;
    if (!p || !key) return;
    for (i = 0; i < p->count;) {
        if (strcmp(p->items[i].key, key) == 0) {
            for (k = i; k + 1 < p->count; k++) p->items[k] = p->items[k + 1];
            p->count--;
        } else {
            i++;
        }
    }
}

const char *ph_props_find_last_str(const ph_props *p, const char *key) {
    int i;
    if (!p || !key) return NULL;
    for (i = p->count - 1; i >= 0; i--) {
        const ph_prop *it = &p->items[i];
        if (it->type == PH_T_STR && strcmp(it->key, key) == 0)
            return it->val.str;
    }
    return NULL;
}

int ph_denylist_has(const char (*list)[PH_KEY_CAP], int count, const char *key) {
    int i;
    if (!key) return 0;
    for (i = 0; i < count; i++)
        if (strcmp(list[i], key) == 0) return 1;
    return 0;
}

void ph_apply_denylist(ph_props *p, const char (*list)[PH_KEY_CAP], int count) {
    int i;
    for (i = 0; i < count; i++) ph_props_remove_key(p, list[i]);
}
