/*
 * ph_jsonval.h — a minimal read-only JSON parser (DOM).
 *
 * The rest of the SDK only ever *writes* JSON, but feature-flag evaluation has
 * to *read* the /flags/ response, so this adds just enough parsing: a small
 * recursive-descent parser into an owned node tree, navigated by key/index.
 * Used off the hot path (init / sender), so it allocates freely.
 */
#ifndef PH_JSONVAL_H
#define PH_JSONVAL_H

#include <stddef.h>

typedef enum ph_jv_type {
    PH_JV_NULL = 0,
    PH_JV_BOOL,
    PH_JV_NUM,
    PH_JV_STR,
    PH_JV_ARR,
    PH_JV_OBJ
} ph_jv_type;

typedef struct ph_jv ph_jv;

/* Parse `n` bytes of JSON. Returns an owned tree (free with ph_jv_free) or NULL
 * on malformed input / OOM. */
ph_jv *ph_jv_parse(const char *s, size_t n);
void ph_jv_free(ph_jv *v);

/* PH_JV_NULL for a NULL pointer, so callers can chain ph_jv_get without checks. */
ph_jv_type ph_jv_type_of(const ph_jv *v);
int ph_jv_bool(const ph_jv *v);       /* BOOL value; 0 otherwise */
double ph_jv_num(const ph_jv *v);     /* NUM value; 0 otherwise */
const char *ph_jv_str(const ph_jv *v); /* NUL-terminated (STR), else NULL */

const ph_jv *ph_jv_get(const ph_jv *v, const char *key); /* OBJ member or NULL */
int ph_jv_len(const ph_jv *v);                  /* ARR/OBJ element count */
const ph_jv *ph_jv_at(const ph_jv *v, int i);   /* ARR element or NULL */
const char *ph_jv_key_at(const ph_jv *v, int i); /* OBJ key at i, or NULL */
const ph_jv *ph_jv_val_at(const ph_jv *v, int i); /* OBJ value at i, or NULL */

#endif /* PH_JSONVAL_H */
