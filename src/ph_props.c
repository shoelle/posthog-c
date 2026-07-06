/*
 * ph_props.c — the caller-facing property builder and the compact packer that
 * turns a ph_props into the bytes stored in a ring slot.
 *
 * The public setters write into a POD ph_props the caller owns (usually on the
 * stack). ph_pack_props / ph_pack_str_entry serialize those into the fixed
 * event blob at capture time — no allocation, and any entry that would overflow
 * the blob is skipped rather than truncated mid-value.
 */
#include "ph_internal.h"

#include <string.h>

/* Copy src into dst[cap] (NUL-terminated). Returns 1 if it had to truncate. */
static int copy_capped(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (cap == 0) return 1;
    if (!src) src = "";
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
    return src[i] != '\0';
}

void ph_props_init(ph_props *p) {
    if (!p) return;
    p->count = 0;
    p->dropped = 0;
}

static ph_prop *next_slot(ph_props *p) {
    if (p->count >= PH_MAX_PROPS) {
        p->dropped++;
        return NULL;
    }
    return &p->items[p->count];
}

ph_result ph_props_set_str(ph_props *p, const char *key, const char *val) {
    ph_prop *it;
    int trunc = 0;
    if (!p || !key || !key[0]) {
        if (p) p->dropped++;
        return PH_ERR_BADARG;
    }
    it = next_slot(p);
    if (!it) return PH_ERR_FULL;
    trunc |= copy_capped(it->key, PH_KEY_CAP, key);
    it->type = PH_T_STR;
    trunc |= copy_capped(it->val.str, PH_VAL_CAP, val ? val : "");
    p->count++;
    return trunc ? PH_ERR_TRUNCATED : PH_OK;
}

ph_result ph_props_set_double(ph_props *p, const char *key, double val) {
    ph_prop *it;
    int trunc = 0;
    if (!p || !key || !key[0]) {
        if (p) p->dropped++;
        return PH_ERR_BADARG;
    }
    it = next_slot(p);
    if (!it) return PH_ERR_FULL;
    trunc |= copy_capped(it->key, PH_KEY_CAP, key);
    it->type = PH_T_DOUBLE;
    it->val.dbl = val;
    p->count++;
    return trunc ? PH_ERR_TRUNCATED : PH_OK;
}

ph_result ph_props_set_int(ph_props *p, const char *key, int64_t val) {
    ph_prop *it;
    int trunc = 0;
    if (!p || !key || !key[0]) {
        if (p) p->dropped++;
        return PH_ERR_BADARG;
    }
    it = next_slot(p);
    if (!it) return PH_ERR_FULL;
    trunc |= copy_capped(it->key, PH_KEY_CAP, key);
    it->type = PH_T_INT;
    it->val.i64 = val;
    p->count++;
    return trunc ? PH_ERR_TRUNCATED : PH_OK;
}

ph_result ph_props_set_bool(ph_props *p, const char *key, int val) {
    ph_prop *it;
    int trunc = 0;
    if (!p || !key || !key[0]) {
        if (p) p->dropped++;
        return PH_ERR_BADARG;
    }
    it = next_slot(p);
    if (!it) return PH_ERR_FULL;
    trunc |= copy_capped(it->key, PH_KEY_CAP, key);
    it->type = PH_T_BOOL;
    it->val.boolean = val ? 1 : 0;
    p->count++;
    return trunc ? PH_ERR_TRUNCATED : PH_OK;
}

/* --- Packing ---------------------------------------------------------- */

/* Write one entry (4-byte header + key + value) if it fits. Returns bytes
 * written, or 0 if the entry could not fit in `cap`. */
static size_t pack_one(char *buf, size_t cap, unsigned char type,
                       const char *key, size_t klen, const void *val,
                       size_t vlen) {
    size_t total = 4 + klen + vlen;
    if (klen > 255 || vlen > 0xFFFF || total > cap) return 0;
    buf[0] = (char)type;
    buf[1] = (char)(unsigned char)klen;
    buf[2] = (char)(unsigned char)(vlen & 0xFF);
    buf[3] = (char)(unsigned char)((vlen >> 8) & 0xFF);
    memcpy(buf + 4, key, klen);
    if (vlen) memcpy(buf + 4 + klen, val, vlen);
    return total;
}

size_t ph_pack_str_entry(char *buf, size_t cap, unsigned char packed_type,
                         const char *key, const char *val) {
    if (!key) return 0;
    if (!val) val = "";
    return pack_one(buf, cap, packed_type, key, strlen(key), val, strlen(val));
}

/* --- Unpacking (sender-side scrub path) ------------------------------- */

int ph_blob_next(const char **cur, const char *end, unsigned char *type,
                 const char **key, size_t *klen, const char **val,
                 size_t *vlen) {
    const char *p = *cur;
    size_t kl, vl;
    if (p + 4 > end) return 0;
    *type = (unsigned char)p[0];
    kl = (unsigned char)p[1];
    vl = (size_t)((unsigned char)p[2]) | ((size_t)((unsigned char)p[3]) << 8);
    if (p + 4 + kl + vl > end) return 0;
    *key = p + 4;
    *klen = kl;
    *val = p + 4 + kl;
    *vlen = vl;
    *cur = p + 4 + kl + vl;
    return 1;
}

void ph_unpack_props(const char *blob, size_t len, ph_props *out) {
    const char *cur = blob;
    const char *end = blob + len;
    unsigned char type;
    const char *key, *val;
    size_t klen, vlen;
    ph_props_init(out);
    while (out->count < PH_MAX_PROPS &&
           ph_blob_next(&cur, end, &type, &key, &klen, &val, &vlen)) {
        ph_prop *it = &out->items[out->count];
        size_t kl = klen < PH_KEY_CAP - 1 ? klen : PH_KEY_CAP - 1;
        if (type == PH_PK_GROUP) continue; /* $groups handled separately */
        memcpy(it->key, key, kl);
        it->key[kl] = '\0';
        it->type = type;
        switch (type) {
            case PH_T_STR: {
                size_t vl = vlen < PH_VAL_CAP - 1 ? vlen : PH_VAL_CAP - 1;
                memcpy(it->val.str, val, vl);
                it->val.str[vl] = '\0';
                break;
            }
            case PH_T_DOUBLE: {
                double d = 0;
                if (vlen == sizeof(double)) memcpy(&d, val, sizeof(double));
                it->val.dbl = d;
                break;
            }
            case PH_T_INT: {
                int64_t i = 0;
                if (vlen == sizeof(int64_t)) memcpy(&i, val, sizeof(int64_t));
                it->val.i64 = i;
                break;
            }
            case PH_T_BOOL:
                it->val.boolean = (vlen >= 1 && val[0]) ? 1 : 0;
                break;
            default:
                continue; /* unknown type: skip without committing */
        }
        out->count++;
    }
}

size_t ph_pack_props(const ph_props *p, char *buf, size_t cap) {
    size_t off = 0;
    int i;
    if (!p) return 0;
    for (i = 0; i < p->count; i++) {
        const ph_prop *it = &p->items[i];
        size_t klen = strlen(it->key);
        size_t n = 0;
        switch (it->type) {
            case PH_T_STR:
                n = pack_one(buf + off, cap - off, PH_T_STR, it->key, klen,
                             it->val.str, strlen(it->val.str));
                break;
            case PH_T_DOUBLE:
                n = pack_one(buf + off, cap - off, PH_T_DOUBLE, it->key, klen,
                             &it->val.dbl, sizeof(double));
                break;
            case PH_T_INT:
                n = pack_one(buf + off, cap - off, PH_T_INT, it->key, klen,
                             &it->val.i64, sizeof(int64_t));
                break;
            case PH_T_BOOL: {
                unsigned char b = it->val.boolean ? 1 : 0;
                n = pack_one(buf + off, cap - off, PH_T_BOOL, it->key, klen, &b, 1);
                break;
            }
            default:
                break;
        }
        /* n == 0 means this entry didn't fit; skip it and keep trying smaller
         * later entries rather than aborting the whole blob. */
        off += n;
    }
    return off;
}
