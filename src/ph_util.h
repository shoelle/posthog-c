/*
 * ph_util.h - small ph_props helpers shared by the native and wasm backends.
 *
 * Both backends manipulate the public ph_props POD the same way: copy a capped
 * string, append a typed value, remove a key, find the last string for a key,
 * apply a denylist. Keeping one copy here removes byte-identical duplication and
 * closes a native/wasm parity-drift gap. Depends only on the public header, so
 * it compiles into the wasm module (which does not pull in ph_internal.h).
 */
#ifndef PH_UTIL_H
#define PH_UTIL_H

#include "posthog.h"

#include <stddef.h>

/* Copy src into dst (cap bytes incl. NUL), truncating to fit. dst is always
 * NUL-terminated; a NULL src yields "". */
void ph_copy_capped(char *dst, size_t cap, const char *src);

/* Append one typed value from src to dst (dispatches on src->type). */
void ph_copy_prop_value(ph_props *dst, const ph_prop *src);

/* Build a fixed-cap merged set with explicit event properties first, then
 * non-shadowed super properties. This makes explicit values win without a
 * privacy-hook round trip changing which entries survive the public cap. */
void ph_props_merge(ph_props *out, const ph_props *explicit_props,
                    const ph_props *super_props);

/* Remove every entry whose key equals `key` (in place). NULL p/key are no-ops. */
void ph_props_remove_key(ph_props *p, const char *key);

/* Value of the last PH_T_STR entry with `key`, or NULL if none. */
const char *ph_props_find_last_str(const ph_props *p, const char *key);

/* 1 if `key` is in `list` (a `count`-entry array of PH_KEY_CAP-byte strings). */
int ph_denylist_has(const char (*list)[PH_KEY_CAP], int count, const char *key);

/* Remove every key listed in `list` (count entries) from `p`. */
void ph_apply_denylist(ph_props *p, const char (*list)[PH_KEY_CAP], int count);

#endif /* PH_UTIL_H */
