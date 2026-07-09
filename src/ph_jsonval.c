#include "ph_jsonval.h"

#include <stdlib.h>
#include <string.h>

struct ph_jv {
    ph_jv_type type;
    union {
        int b;
        double num;
        char *str; /* STR: owned, unescaped, NUL-terminated */
        struct {
            ph_jv **items;
            int count;
        } arr;
        struct {
            char **keys;
            ph_jv **vals;
            int count;
        } obj;
    } u;
};

typedef struct {
    const char *p;
    const char *end;
} cursor;

/* Cap recursion so a deeply nested (possibly malicious) /flags/ body can't blow
 * the stack — real responses nest only a handful deep. Tunable via -D. */
#ifndef PH_JV_MAX_DEPTH
#define PH_JV_MAX_DEPTH 128
#endif

static ph_jv *parse_value(cursor *c, int depth);

static void skip_ws(cursor *c) {
    while (c->p < c->end) {
        char ch = *c->p;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') c->p++;
        else break;
    }
}

static ph_jv *node_new(ph_jv_type t) {
    ph_jv *v = (ph_jv *)calloc(1, sizeof(ph_jv));
    if (v) v->type = t;
    return v;
}

void ph_jv_free(ph_jv *v) {
    int i;
    if (!v) return;
    switch (v->type) {
        case PH_JV_STR:
            free(v->u.str);
            break;
        case PH_JV_ARR:
            for (i = 0; i < v->u.arr.count; i++) ph_jv_free(v->u.arr.items[i]);
            free(v->u.arr.items);
            break;
        case PH_JV_OBJ:
            for (i = 0; i < v->u.obj.count; i++) {
                free(v->u.obj.keys[i]);
                ph_jv_free(v->u.obj.vals[i]);
            }
            free(v->u.obj.keys);
            free(v->u.obj.vals);
            break;
        default:
            break;
    }
    free(v);
}

/* Encode a Unicode code point as UTF-8 into out (>= 4 bytes); returns length. */
static int utf8_encode(unsigned cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char ch = p[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* Parse a JSON string starting at the opening quote. Returns a malloc'd,
 * unescaped, NUL-terminated string, or NULL on error. Advances the cursor. */
static char *parse_string_raw(cursor *c) {
    const char *p = c->p;
    char *out;
    size_t cap, len = 0;
    if (p >= c->end || *p != '"') return NULL;
    p++;
    cap = 16;
    out = (char *)malloc(cap);
    if (!out) return NULL;

    while (p < c->end) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"') {
            p++;
            c->p = p;
            out[len] = '\0';
            return out;
        }
        if (len + 4 >= cap) {
            char *n2;
            cap *= 2;
            n2 = (char *)realloc(out, cap);
            if (!n2) {
                free(out);
                return NULL;
            }
            out = n2;
        }
        if (ch == '\\') {
            p++;
            if (p >= c->end) break;
            switch (*p) {
                case '"': out[len++] = '"'; p++; break;
                case '\\': out[len++] = '\\'; p++; break;
                case '/': out[len++] = '/'; p++; break;
                case 'b': out[len++] = '\b'; p++; break;
                case 'f': out[len++] = '\f'; p++; break;
                case 'n': out[len++] = '\n'; p++; break;
                case 'r': out[len++] = '\r'; p++; break;
                case 't': out[len++] = '\t'; p++; break;
                case 'u': {
                    unsigned cp;
                    if (p + 5 > c->end || !hex4(p + 1, &cp)) {
                        free(out);
                        return NULL;
                    }
                    p += 5;
                    /* high surrogate: combine with the following low surrogate
                     * into a single code point before encoding to UTF-8 */
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned lo;
                        if (p + 6 <= c->end && p[0] == '\\' && p[1] == 'u' &&
                            hex4(p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    len += (size_t)utf8_encode(cp, out + len);
                    break;
                }
                default:
                    free(out);
                    return NULL;
            }
        } else {
            out[len++] = (char)ch;
            p++;
        }
    }
    free(out);
    return NULL;
}

static ph_jv *parse_string(cursor *c) {
    char *s = parse_string_raw(c);
    ph_jv *v;
    if (!s) return NULL;
    v = node_new(PH_JV_STR);
    if (!v) {
        free(s);
        return NULL;
    }
    v->u.str = s;
    return v;
}

static ph_jv *parse_number(cursor *c) {
    char buf[64];
    const char *start = c->p;
    size_t n;
    ph_jv *v;
    while (c->p < c->end) {
        char ch = *c->p;
        if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' ||
            ch == 'e' || ch == 'E')
            c->p++;
        else
            break;
    }
    n = (size_t)(c->p - start);
    if (n == 0 || n >= sizeof(buf)) return NULL;
    memcpy(buf, start, n);
    buf[n] = '\0';
    v = node_new(PH_JV_NUM);
    if (!v) return NULL;
    v->u.num = strtod(buf, NULL);
    return v;
}

static ph_jv *parse_literal(cursor *c) {
    if (c->end - c->p >= 4 && memcmp(c->p, "true", 4) == 0) {
        ph_jv *v = node_new(PH_JV_BOOL);
        if (v) v->u.b = 1;
        c->p += 4;
        return v;
    }
    if (c->end - c->p >= 5 && memcmp(c->p, "false", 5) == 0) {
        ph_jv *v = node_new(PH_JV_BOOL);
        if (v) v->u.b = 0;
        c->p += 5;
        return v;
    }
    if (c->end - c->p >= 4 && memcmp(c->p, "null", 4) == 0) {
        c->p += 4;
        return node_new(PH_JV_NULL);
    }
    return NULL;
}

static ph_jv *parse_array(cursor *c, int depth) {
    ph_jv *v = node_new(PH_JV_ARR);
    if (!v) return NULL;
    c->p++; /* '[' */
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        return v;
    }
    for (;;) {
        ph_jv *item;
        ph_jv **grown;
        skip_ws(c);
        item = parse_value(c, depth + 1);
        if (!item) {
            ph_jv_free(v);
            return NULL;
        }
        grown = (ph_jv **)realloc(v->u.arr.items, sizeof(ph_jv *) * (size_t)(v->u.arr.count + 1));
        if (!grown) {
            ph_jv_free(item);
            ph_jv_free(v);
            return NULL;
        }
        v->u.arr.items = grown;
        v->u.arr.items[v->u.arr.count++] = item;
        skip_ws(c);
        if (c->p >= c->end) {
            ph_jv_free(v);
            return NULL;
        }
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        if (*c->p == ']') {
            c->p++;
            return v;
        }
        ph_jv_free(v);
        return NULL;
    }
}

static ph_jv *parse_object(cursor *c, int depth) {
    ph_jv *v = node_new(PH_JV_OBJ);
    if (!v) return NULL;
    c->p++; /* '{' */
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return v;
    }
    for (;;) {
        char *key;
        ph_jv *val;
        char **gk;
        ph_jv **gv;
        skip_ws(c);
        key = parse_string_raw(c);
        if (!key) {
            ph_jv_free(v);
            return NULL;
        }
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') {
            free(key);
            ph_jv_free(v);
            return NULL;
        }
        c->p++;
        skip_ws(c);
        val = parse_value(c, depth + 1);
        if (!val) {
            free(key);
            ph_jv_free(v);
            return NULL;
        }
        gk = (char **)realloc(v->u.obj.keys, sizeof(char *) * (size_t)(v->u.obj.count + 1));
        if (gk) v->u.obj.keys = gk;
        gv = (ph_jv **)realloc(v->u.obj.vals, sizeof(ph_jv *) * (size_t)(v->u.obj.count + 1));
        if (gv) v->u.obj.vals = gv;
        if (!gk || !gv) {
            free(key);
            ph_jv_free(val);
            ph_jv_free(v);
            return NULL;
        }
        v->u.obj.keys[v->u.obj.count] = key;
        v->u.obj.vals[v->u.obj.count] = val;
        v->u.obj.count++;
        skip_ws(c);
        if (c->p >= c->end) {
            ph_jv_free(v);
            return NULL;
        }
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        if (*c->p == '}') {
            c->p++;
            return v;
        }
        ph_jv_free(v);
        return NULL;
    }
}

static ph_jv *parse_value(cursor *c, int depth) {
    skip_ws(c);
    if (c->p >= c->end) return NULL;
    if (depth > PH_JV_MAX_DEPTH) return NULL; /* too deeply nested — reject */
    switch (*c->p) {
        case '{': return parse_object(c, depth);
        case '[': return parse_array(c, depth);
        case '"': return parse_string(c);
        case 't':
        case 'f':
        case 'n': return parse_literal(c);
        default: return parse_number(c);
    }
}

ph_jv *ph_jv_parse(const char *s, size_t n) {
    cursor c;
    ph_jv *v;
    if (!s) return NULL;
    c.p = s;
    c.end = s + n;
    v = parse_value(&c, 0);
    return v; /* trailing content is tolerated (responses may have none) */
}

/* --- accessors -------------------------------------------------------- */

ph_jv_type ph_jv_type_of(const ph_jv *v) { return v ? v->type : PH_JV_NULL; }
int ph_jv_bool(const ph_jv *v) { return (v && v->type == PH_JV_BOOL) ? v->u.b : 0; }
double ph_jv_num(const ph_jv *v) { return (v && v->type == PH_JV_NUM) ? v->u.num : 0.0; }
const char *ph_jv_str(const ph_jv *v) { return (v && v->type == PH_JV_STR) ? v->u.str : NULL; }

const ph_jv *ph_jv_get(const ph_jv *v, const char *key) {
    int i;
    if (!v || v->type != PH_JV_OBJ || !key) return NULL;
    for (i = 0; i < v->u.obj.count; i++)
        if (strcmp(v->u.obj.keys[i], key) == 0) return v->u.obj.vals[i];
    return NULL;
}

int ph_jv_len(const ph_jv *v) {
    if (!v) return 0;
    if (v->type == PH_JV_ARR) return v->u.arr.count;
    if (v->type == PH_JV_OBJ) return v->u.obj.count;
    return 0;
}

const ph_jv *ph_jv_at(const ph_jv *v, int i) {
    if (!v || v->type != PH_JV_ARR || i < 0 || i >= v->u.arr.count) return NULL;
    return v->u.arr.items[i];
}

const char *ph_jv_key_at(const ph_jv *v, int i) {
    if (!v || v->type != PH_JV_OBJ || i < 0 || i >= v->u.obj.count) return NULL;
    return v->u.obj.keys[i];
}

const ph_jv *ph_jv_val_at(const ph_jv *v, int i) {
    if (!v || v->type != PH_JV_OBJ || i < 0 || i >= v->u.obj.count) return NULL;
    return v->u.obj.vals[i];
}
